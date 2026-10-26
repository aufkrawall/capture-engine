#include "module_replacement_internal.h"

#include <atomic>
#include <cstdint>
#include <vector>

#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#include "common/logging/log_meter.h"
#include "hook/hooking/inline_hook.h"
#include "hook/hooking/module_export_resolver.h"
#include "hook/overlay/overlay_compat_detail/module_address_cache.h"
#include "hook/overlay/overlay_compat_detail/module_table.h"
#include "hook/runtime/com_object_module.h"
#include "hook/runtime/hook_common.h"

// System32's D3D10/D3D11 runtimes read private DXGI state through the adapter they are given:
// D3D11CreateDevice asks it for an interface only System32's dxgi.dll implements and dereferences
// the answer. While dxgi.dll is replaced (DXVK's, for vkd3d-proton) and these runtimes are not,
// application code that enumerates adapters gets the replacement's, and handing one to the native
// runtime faults in d3d11!CCreateDeviceCache::CAdapterCache::GetWddmVersion (E_NOINTERFACE, then a
// null read). GTA V Enhanced's Epic Online Services overlay does exactly that when it initializes
// its Vulkan overlay (session 20261026_175321). The device-creation entries therefore exchange such
// an adapter for System32's adapter with the same LUID: the same GPU, a runtime pairing that works.
namespace ce::module_replacement::detail {

namespace {

std::atomic<HMODULE> g_systemDxgi{nullptr};
std::atomic<HMODULE> g_replacementDxgi{nullptr};

struct GuardSlot {
    const char* module = nullptr;
    const char* name = nullptr;
    std::atomic<void*> trampoline{nullptr};
    std::atomic<uint32_t> substitutions{0};
    std::atomic<uint32_t> unavailable{0};
};

const char* CallerName(const void* callerAddress, char (&path)[MAX_PATH]) {
    HMODULE caller = nullptr;
    path[0] = '\0';
    ce::overlay_compat::module_address_cache::Lookup(callerAddress, &caller, path, sizeof(path), nullptr);
    const char* base = path;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            base = cursor + 1;
        }
    }
    return base[0] ? base : "unattributed code";
}

// System32's adapter for `luid`, or nullptr. The factory comes from System32's own entry point; a
// call from CE is never forwarded to the replacement (RequesterScope).
IDXGIAdapter* SystemAdapterFor(const LUID& luid) {
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<CreateFactory1>(
        ce::module_export::ResolveAddressDirect(g_systemDxgi.load(std::memory_order_acquire), "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        return nullptr;
    }
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory4* factory4 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory4))) &&
        factory4) {
        if (FAILED(factory4->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter)))) {
            adapter = nullptr;
        }
        factory4->Release();
    }
    for (UINT index = 0; !adapter; ++index) {
        IDXGIAdapter* candidate = nullptr;
        if (factory->EnumAdapters(index, &candidate) == DXGI_ERROR_NOT_FOUND || !candidate) {
            break;
        }
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart &&
            desc.AdapterLuid.HighPart == luid.HighPart) {
            adapter = candidate;
        } else {
            candidate->Release();
        }
    }
    factory->Release();
    return adapter;
}

// The adapter one device-creation call passes on: the caller's, or for an adapter of the replaced
// dxgi.dll System32's adapter with the same LUID, held for the duration of the call.
class NativeAdapter {
public:
    explicit NativeAdapter(IDXGIAdapter* adapter) : adapter_(adapter) {
        const HMODULE replacement = g_replacementDxgi.load(std::memory_order_acquire);
        if (!adapter || !replacement || !ce::com_object_module::IsImplementedBy(adapter, replacement)) {
            return;
        }
        translated_ = true;
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            luid_ = desc.AdapterLuid;
            substitute_ = SystemAdapterFor(luid_);
        }
    }
    ~NativeAdapter() {
        if (substitute_) {
            substitute_->Release();
        }
    }
    NativeAdapter(const NativeAdapter&) = delete;
    NativeAdapter& operator=(const NativeAdapter&) = delete;

    bool Translated() const { return translated_; }
    bool Unavailable() const { return translated_ && !substitute_; }
    IDXGIAdapter* Get() const { return substitute_ ? substitute_ : adapter_; }
    const LUID& Luid() const { return luid_; }

private:
    IDXGIAdapter* adapter_ = nullptr;
    IDXGIAdapter* substitute_ = nullptr;
    bool translated_ = false;
    LUID luid_ = {};
};

void LogTranslation(GuardSlot& slot, const NativeAdapter& native, const void* callerAddress) {
    std::atomic<uint32_t>& counter = native.Unavailable() ? slot.unavailable : slot.substitutions;
    const uint32_t count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!ce::log_meter::ShouldLogCadence(count, 4, 64)) {
        return;
    }
    char path[MAX_PATH];
    const char* caller = CallerName(callerAddress, path);
    if (native.Unavailable()) {
        HookLogImportant("ModuleReplacement: %s!%s from %s received an adapter of the replaced dxgi.dll and no System32 "
                         "adapter has its LUID %08lX:%08lX; failing the call with DXGI_ERROR_UNSUPPORTED instead of "
                         "letting the native runtime fault (#%u)",
                         slot.module, slot.name, caller, static_cast<unsigned long>(native.Luid().HighPart),
                         static_cast<unsigned long>(native.Luid().LowPart), count);
        return;
    }
    HookLogImportant("ModuleReplacement: %s!%s from %s received an adapter of the replaced dxgi.dll; passed System32's "
                     "adapter with the same LUID %08lX:%08lX to the native runtime (#%u)",
                     slot.module, slot.name, caller, static_cast<unsigned long>(native.Luid().HighPart),
                     static_cast<unsigned long>(native.Luid().LowPart), count);
}

template <size_t Index, typename Signature>
struct AdapterGuard;

// Every guarded entry takes the adapter first and returns an HRESULT.
template <size_t Index, typename... Rest>
struct AdapterGuard<Index, HRESULT(WINAPI*)(IDXGIAdapter*, Rest...)> {
    static GuardSlot slot;
    static HRESULT WINAPI Detour(IDXGIAdapter* adapter, Rest... rest) {
        using Function = HRESULT(WINAPI*)(IDXGIAdapter*, Rest...);
        const auto original = reinterpret_cast<Function>(slot.trampoline.load(std::memory_order_acquire));
        const NativeAdapter native(adapter);
        if (native.Translated()) {
            LogTranslation(slot, native, __builtin_return_address(0));
            if (native.Unavailable()) {
                return DXGI_ERROR_UNSUPPORTED;
            }
        }
        return original(native.Get(), rest...);
    }
};

template <size_t Index, typename... Rest>
GuardSlot AdapterGuard<Index, HRESULT(WINAPI*)(IDXGIAdapter*, Rest...)>::slot;

struct GuardDefinition {
    const char* module;
    const char* name;
    void* detour;
    GuardSlot* slot;
};

template <size_t Index, typename Signature>
GuardDefinition Guard(const char* module, const char* name) {
    return {module, name, reinterpret_cast<void*>(&AdapterGuard<Index, Signature>::Detour),
            &AdapterGuard<Index, Signature>::slot};
}

const std::vector<GuardDefinition>& Definitions() {
    static const std::vector<GuardDefinition> kDefinitions = {
        Guard<0, decltype(&D3D11CreateDevice)>("d3d11.dll", "D3D11CreateDevice"),
        Guard<1, decltype(&D3D11CreateDeviceAndSwapChain)>("d3d11.dll", "D3D11CreateDeviceAndSwapChain"),
        Guard<2, decltype(&D3D10CreateDevice)>("d3d10.dll", "D3D10CreateDevice"),
        Guard<3, decltype(&D3D10CreateDeviceAndSwapChain)>("d3d10.dll", "D3D10CreateDeviceAndSwapChain"),
        Guard<4, decltype(&D3D10CreateDevice1)>("d3d10_1.dll", "D3D10CreateDevice1"),
        Guard<5, decltype(&D3D10CreateDeviceAndSwapChain1)>("d3d10_1.dll", "D3D10CreateDeviceAndSwapChain1"),
    };
    return kDefinitions;
}

void PublishTrampoline(void* trampoline, void* context) {
    static_cast<std::atomic<void*>*>(context)->store(trampoline, std::memory_order_release);
}

}  // namespace

const char* const* NativeAdapterRuntimeNames(size_t* count) {
    static const char* const kNames[] = {"d3d11.dll", "d3d10.dll", "d3d10_1.dll"};
    *count = sizeof(kNames) / sizeof(kNames[0]);
    return kNames;
}

void PublishDxgiPairing(HMODULE systemDxgi, HMODULE replacementDxgi) {
    g_systemDxgi.store(systemDxgi, std::memory_order_release);
    g_replacementDxgi.store(replacementDxgi, std::memory_order_release);
}

AdapterGuardTally InstallNativeAdapterGuards(const char* baseName, HMODULE runtime) {
    AdapterGuardTally tally;
    std::vector<InlineHook::PublishedHookSpec> specs;
    std::vector<const GuardDefinition*> definitions;
    for (const GuardDefinition& definition : Definitions()) {
        if (!ce::overlay_compat::detail::EqualsInsensitive(definition.module, baseName)) {
            continue;
        }
        // The runtime's own export table: a GetProcAddress interceptor (an overlay's) would name its
        // detour instead of the code every caller, including that detour, finally reaches.
        void* entry = ce::module_export::ResolveAddressDirect(runtime, definition.name);
        if (!entry) {
            ++tally.missing;
            continue;
        }
        definition.slot->module = definition.module;
        definition.slot->name = definition.name;
        InlineHook::PublishedHookSpec spec;
        spec.target = entry;
        spec.detour = definition.detour;
        spec.publisher = &PublishTrampoline;
        spec.publisherContext = &definition.slot->trampoline;
        specs.push_back(spec);
        definitions.push_back(&definition);
    }
    std::vector<void*> trampolines(specs.size(), nullptr);
    for (size_t i = 0; i < specs.size(); ++i) {
        specs[i].outTrampoline = &trampolines[i];
    }
    if (!specs.empty()) {
        InlineHook::InstallPublishedBatch(specs.data(), specs.size());
    }
    for (size_t i = 0; i < specs.size(); ++i) {
        if (specs[i].installed) {
            ++tally.installed;
        } else if (specs[i].leftToForeignOwner) {
            ++tally.leftToForeignOwner;
        } else {
            ++tally.failed;
        }
        if (!specs[i].installed) {
            HookLogImportant("ModuleReplacement: %s!%s is not guarded against adapters of the replaced dxgi.dll (%s)",
                             definitions[i]->module, definitions[i]->name,
                             specs[i].leftToForeignOwner ? "another component owns its entry" : "install failed");
        }
    }
    return tally;
}

}  // namespace ce::module_replacement::detail
