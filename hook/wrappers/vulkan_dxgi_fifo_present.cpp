#include "vulkan_dxgi_fifo_present.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

#include <dxgi1_6.h>

#include "../common/dxgi_shared.h"
#include "../common/hook_common.h"
#include "../common/overlay_compat.h"
#include "../common/vulkan_dxgi_fifo_policy.h"
#include "iat_hook.h"
#include "vtable_hook.h"
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

constexpr size_t kMaxFactoryVtables = 12;
constexpr size_t kMaxSwapchainVtables = 12;

struct FactoryVtableHooks {
    std::atomic<void**> vtable{nullptr};
    std::atomic<CreateSwapChainFn> createSwapChain{nullptr};
    std::atomic<CreateSwapChainForHwndFn> createForHwnd{nullptr};
    std::atomic<CreateSwapChainForCoreWindowFn> createForCoreWindow{nullptr};
    std::atomic<CreateSwapChainForCompositionFn> createForComposition{nullptr};
};

struct SwapchainVtableHooks {
    std::atomic<void**> vtable{nullptr};
    std::atomic<PresentFn> present{nullptr};
    std::atomic<Present1Fn> present1{nullptr};
};

std::array<FactoryVtableHooks, kMaxFactoryVtables> g_factoryHooks;
std::array<SwapchainVtableHooks, kMaxSwapchainVtables> g_swapchainHooks;
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

template <typename Record, size_t Count>
Record* FindRecord(std::array<Record, Count>& records, void** vtable) {
    if (!vtable)
        return nullptr;
    for (Record& record : records) {
        if (record.vtable.load(std::memory_order_acquire) == vtable)
            return &record;
    }
    return nullptr;
}

template <typename Record, size_t Count>
Record* FindOrReserveRecord(std::array<Record, Count>& records, void** vtable) {
    if (Record* existing = FindRecord(records, vtable))
        return existing;
    for (Record& record : records) {
        void** expected = nullptr;
        if (record.vtable.compare_exchange_strong(expected, vtable, std::memory_order_acq_rel))
            return &record;
    }
    return nullptr;
}

template <typename Fn>
bool InstallSlot(void** vtable, size_t slot, void* detour, std::atomic<Fn>& originalOut, const char* label) {
    void* observed = vtable[slot];
    if (!observed)
        return false;

    // Publish the predecessor before the slot becomes callable through CE.
    // VTableHook's compare/exchange then verifies that the same predecessor is
    // still current before installing the detour.
    originalOut.store(reinterpret_cast<Fn>(observed), std::memory_order_release);
    void* captured = observed;
    const VTableHook::Status status = VTableHook::Create(reinterpret_cast<void*>(&vtable[slot]), detour, &captured);
    if (status != VTableHook::Success) {
        originalOut.store(nullptr, std::memory_order_release);
        HookLogImportant(
            "Vulkan DXGI FIFO: could not install %s vtable[%zu] hook "
            "(vtable=%p status=%s)",
            label, slot, vtable, VTableHook::StatusToString(status));
        return false;
    }
    originalOut.store(reinterpret_cast<Fn>(captured), std::memory_order_release);
    return true;
}

template <typename Fn, typename Record, size_t Count>
Fn FindOriginal(std::array<Record, Count>& records, const void* object, std::atomic<Fn> Record::* member) {
    if (Record* record = FindRecord(records, GetVtable(object)))
        return (record->*member).load(std::memory_order_acquire);
    return nullptr;
}

void LogOriginalOwner(const char* label, const void* original) {
    char modulePath[MAX_PATH] = {};
    ce::overlay_compat::TryGetModulePathFromCodeAddress(original, modulePath, sizeof(modulePath));
    HookLogImportant("Vulkan DXGI FIFO: installed %s hook (predecessor=%p owner=%s)", label, original,
                     modulePath[0] ? modulePath : "unresolved");
}

bool InstallSwapchainVtable(IDXGISwapChain* swapchain, bool includePresent1, const char* source) {
    void** vtable = GetVtable(swapchain);
    if (!vtable)
        return false;

    std::lock_guard<std::mutex> lock(g_installMutex);
    SwapchainVtableHooks* record = FindOrReserveRecord(g_swapchainHooks, vtable);
    if (!record) {
        HookLogImportant("Vulkan DXGI FIFO: swapchain-vtable registry full (vtable=%p source=%s)", vtable,
                         source ? source : "unknown");
        return false;
    }

    bool installed = record->present.load(std::memory_order_acquire) != nullptr;
    if (!installed) {
        installed = InstallSlot(vtable, 8, reinterpret_cast<void*>(&DetourPresent), record->present, "Present");
        if (installed)
            LogOriginalOwner("Present", reinterpret_cast<const void*>(record->present.load(std::memory_order_acquire)));
    }
    if (includePresent1 && record->present1.load(std::memory_order_acquire) == nullptr) {
        if (InstallSlot(vtable, 22, reinterpret_cast<void*>(&DetourPresent1), record->present1, "Present1")) {
            installed = true;
            LogOriginalOwner("Present1",
                             reinterpret_cast<const void*>(record->present1.load(std::memory_order_acquire)));
        }
    }
    return installed;
}

bool InstallFactoryVtable(IUnknown* factoryInterface, bool includeFactory2, const char* source) {
    void** vtable = GetVtable(factoryInterface);
    if (!vtable)
        return false;

    std::lock_guard<std::mutex> lock(g_installMutex);
    FactoryVtableHooks* record = FindOrReserveRecord(g_factoryHooks, vtable);
    if (!record) {
        HookLogImportant("Vulkan DXGI FIFO: factory-vtable registry full (vtable=%p source=%s)", vtable,
                         source ? source : "unknown");
        return false;
    }

    bool installed = false;
    if (!record->createSwapChain.load(std::memory_order_acquire)) {
        installed |= InstallSlot(vtable, 10, reinterpret_cast<void*>(&DetourCreateSwapChain), record->createSwapChain,
                                 "CreateSwapChain");
    }
    if (includeFactory2 && !record->createForHwnd.load(std::memory_order_acquire)) {
        installed |= InstallSlot(vtable, 15, reinterpret_cast<void*>(&DetourCreateSwapChainForHwnd),
                                 record->createForHwnd, "CreateSwapChainForHwnd");
    }
    if (includeFactory2 && !record->createForCoreWindow.load(std::memory_order_acquire)) {
        installed |= InstallSlot(vtable, 16, reinterpret_cast<void*>(&DetourCreateSwapChainForCoreWindow),
                                 record->createForCoreWindow, "CreateSwapChainForCoreWindow");
    }
    if (includeFactory2 && !record->createForComposition.load(std::memory_order_acquire)) {
        installed |= InstallSlot(vtable, 24, reinterpret_cast<void*>(&DetourCreateSwapChainForComposition),
                                 record->createForComposition, "CreateSwapChainForComposition");
    }

    if (installed) {
        HookLogImportant(
            "Vulkan DXGI FIFO: captured real factory vtable=%p factory2=%d "
            "source=%s; descriptors and factory object remain untouched",
            vtable, includeFactory2 ? 1 : 0, source ? source : "unknown");
    }
    return installed;
}

void InstallSwapchainHooks(IDXGISwapChain* swapchain, const char* source) {
    if (!swapchain)
        return;

    InstallSwapchainVtable(swapchain, false, source);
    IDXGISwapChain1* swapchain1 = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1))) && swapchain1) {
        InstallSwapchainVtable(swapchain1, true, source);
        swapchain1->Release();
    }
}

template <typename Fn>
HRESULT MissingOriginal(const char* label, Fn) {
    static std::atomic<uint32_t> occurrence{0};
    const uint32_t count = occurrence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 8 || (count % 256) == 0) {
        HookLogImportant("Vulkan DXGI FIFO: missing predecessor for %s (occurrence=%u)", label, count);
    }
    return DXGI_ERROR_INVALID_CALL;
}

bool ShouldForceFifoNow() {
    const auto& graphics = GetActiveGraphicsConfig();
    return ce::vulkan_dxgi_fifo_policy::ShouldForceFinalDxgiFifo(g_armed.load(std::memory_order_acquire),
                                                                 DXGIShared::IsVulkanActive(), HookIsShuttingDown(),
                                                                 graphics.vsyncMode);
}

void ApplyAndLogPresentParameters(const char* label, UINT& syncInterval, UINT& flags) {
    const UINT incomingSyncInterval = syncInterval;
    const UINT incomingFlags = flags;
    const bool forceFifo = ShouldForceFifoNow();
    ce::vulkan_dxgi_fifo_policy::ApplyFinalDxgiFifoParameters(forceFifo, syncInterval, flags);

    static std::atomic<uint32_t> occurrence{0};
    const uint32_t count = occurrence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 24 || (count % 1024) == 0) {
        HookLogImportant(
            "Vulkan DXGI FIFO: final %s #%u force=%d SyncInterval=%u->%u "
            "Flags=0x%X->0x%X",
            label, count, forceFifo ? 1 : 0, incomingSyncInterval, syncInterval, incomingFlags, flags);
    }
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                                IDXGISwapChain** swapchain) {
    CreateSwapChainFn original = FindOriginal(g_factoryHooks, factory, &FactoryVtableHooks::createSwapChain);
    if (!original)
        return MissingOriginal("CreateSwapChain", original);
    const HRESULT result = original(factory, device, desc, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain)
        InstallSwapchainHooks(*swapchain, "CreateSwapChain");
    return result;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
                                                       const DXGI_SWAP_CHAIN_DESC1* desc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                       IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapchain) {
    CreateSwapChainForHwndFn original = FindOriginal(g_factoryHooks, factory, &FactoryVtableHooks::createForHwnd);
    if (!original)
        return MissingOriginal("CreateSwapChainForHwnd", original);
    const HRESULT result = original(factory, device, window, desc, fullscreenDesc, restrictToOutput, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain)
        InstallSwapchainHooks(*swapchain, "CreateSwapChainForHwnd");
    return result;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForCoreWindow(IDXGIFactory2* factory, IUnknown* device, IUnknown* window,
                                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                                             IDXGIOutput* restrictToOutput,
                                                             IDXGISwapChain1** swapchain) {
    CreateSwapChainForCoreWindowFn original =
        FindOriginal(g_factoryHooks, factory, &FactoryVtableHooks::createForCoreWindow);
    if (!original)
        return MissingOriginal("CreateSwapChainForCoreWindow", original);
    const HRESULT result = original(factory, device, window, desc, restrictToOutput, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain)
        InstallSwapchainHooks(*swapchain, "CreateSwapChainForCoreWindow");
    return result;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForComposition(IDXGIFactory2* factory, IUnknown* device,
                                                              const DXGI_SWAP_CHAIN_DESC1* desc,
                                                              IDXGIOutput* restrictToOutput,
                                                              IDXGISwapChain1** swapchain) {
    CreateSwapChainForCompositionFn original =
        FindOriginal(g_factoryHooks, factory, &FactoryVtableHooks::createForComposition);
    if (!original)
        return MissingOriginal("CreateSwapChainForComposition", original);
    const HRESULT result = original(factory, device, desc, restrictToOutput, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain)
        InstallSwapchainHooks(*swapchain, "CreateSwapChainForComposition");
    return result;
}

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags) {
    PresentFn original = FindOriginal(g_swapchainHooks, swapchain, &SwapchainVtableHooks::present);
    if (!original)
        return MissingOriginal("Present", original);
    ApplyAndLogPresentParameters("Present", syncInterval, flags);
    return original(swapchain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* parameters) {
    Present1Fn original = FindOriginal(g_swapchainHooks, swapchain, &SwapchainVtableHooks::present1);
    if (!original)
        return MissingOriginal("Present1", original);
    ApplyAndLogPresentParameters("Present1", syncInterval, flags);
    return original(swapchain, syncInterval, flags, parameters);
}

}  // namespace

void RegisterDynamicFactoryHooks(bool vulkanLayerModuleLoaded) {
    const auto& graphics = GetActiveGraphicsConfig();
    if (!ce::vulkan_dxgi_fifo_policy::ShouldArmFinalDxgiPresent(vulkanLayerModuleLoaded, graphics.vsyncMode)) {
        return;
    }

    const bool firstArm = !g_armed.exchange(true, std::memory_order_acq_rel);

    // Re-registering is intentional. If renderer ownership became observable
    // late, an earlier speculative D3D initialization may have replaced these
    // three dynamic-router entries before standing down.
    IATHook::RegisterDynamicHookFiltered("CreateDXGIFactory", reinterpret_cast<void*>(&Wrapped_CreateDXGIFactory),
                                         nullptr, &IsSystemDxgiModule);
    IATHook::RegisterDynamicHookFiltered("CreateDXGIFactory1", reinterpret_cast<void*>(&Wrapped_CreateDXGIFactory1),
                                         nullptr, &IsSystemDxgiModule);
    IATHook::RegisterDynamicHookFiltered("CreateDXGIFactory2", reinterpret_cast<void*>(&Wrapped_CreateDXGIFactory2),
                                         nullptr, &IsSystemDxgiModule);
    if (firstArm) {
        HookLogImportant(
            "Vulkan DXGI FIFO: armed narrow system-DXGI factory interception for "
            "final vblank presentation");
    }
}

bool MaybeInstallFactoryHooks(IUnknown* factory, const char* source) {
    if (!g_armed.load(std::memory_order_acquire) || !factory)
        return false;

    // Query each interface before indexing its methods. A base IDXGIFactory
    // interface is only guaranteed through vtable[11]; the Factory2 creation
    // methods at [15], [16], and [24] belong to its queried interface.
    IDXGIFactory* factory0 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory0))) && factory0) {
        InstallFactoryVtable(factory0, false, source);
        factory0->Release();
    }

    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2))) && factory2) {
        InstallFactoryVtable(factory2, true, source);
        factory2->Release();
    }
    return true;
}

}  // namespace ce::vulkan_dxgi_fifo
