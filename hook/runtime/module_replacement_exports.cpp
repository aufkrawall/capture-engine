#include "module_replacement_internal.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_3.h>
#include <dxgidebug.h>

#include "common/logging/log_meter.h"
#include "hook/hooking/inline_hook.h"
#include "hook/overlay/overlay_compat_detail/module_address_cache.h"
#include "hook/overlay/overlay_compat_detail/module_table.h"
#include "hook/runtime/hook_common.h"

// Entry-point forwarders for a superseded runtime image (see ShouldForwardEntryCall). Each known
// export gets a detour with its SDK signature, including x86's stdcall convention. d3d9,
// d3d8 and ddraw are not listed: CE's own D3D9/DirectDraw hooks already own those entries.
namespace ce::module_replacement::detail {

namespace {

thread_local unsigned g_nativeRuntimeDepth = 0;

// A proxy may tail-call the native runtime, erasing its own return address. Mark execution
// through both entries so that even an import bound directly to the proxy keeps native forwards.
struct NativeRuntimeScope {
    NativeRuntimeScope() { ++g_nativeRuntimeDepth; }
    ~NativeRuntimeScope() { --g_nativeRuntimeDepth; }
};

struct EntrySlot {
    EntrySlot() noexcept = default;

    const char* module = nullptr;
    const char* name = nullptr;
    std::atomic<void*> trampoline{nullptr};
    std::atomic<void*> replacement{nullptr};
    std::atomic<uint32_t> forwardedCalls{0};
    ce::log_meter::ChangeGate forwardedLog{5000};
};

void LogForwardedCall(EntrySlot& slot, const void* callerAddress) {
    const uint32_t count = slot.forwardedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    HMODULE caller = nullptr;
    char path[MAX_PATH] = {};
    ce::overlay_compat::module_address_cache::Lookup(callerAddress, &caller, path, sizeof(path), nullptr);
    // Keyed by caller: a new module taking the replacement is the event that matters, as when an
    // overlay's factory call fed a translated adapter to native D3D11 (GTA, session 20261026_175321).
    const auto verdict = slot.forwardedLog.Observe(ce::log_meter::FieldKey(caller), GetTickCount64());
    if (!verdict) {
        return;
    }
    const char* base = path;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            base = cursor + 1;
        }
    }
    HookLog("ModuleReplacement: %s!%s entry called from %s -> replacement (call #%u)%s", slot.module, slot.name,
            base[0] ? base : "unattributed code", count, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
}

void* SelectEntryTarget(EntrySlot& slot, const void* callerAddress) {
    void* replacement = slot.replacement.load(std::memory_order_acquire);
    if (replacement && g_nativeRuntimeDepth == 0 && ForwardsEntryCallFrom(callerAddress)) {
        LogForwardedCall(slot, callerAddress);
        return replacement;
    }
    return slot.trampoline.load(std::memory_order_acquire);
}

template <size_t Index, typename Signature>
struct EntryForwarder;

template <size_t Index, typename Result, typename... Args>
struct EntryForwarder<Index, Result(WINAPI*)(Args...)> {
    static EntrySlot slot;
    static Result WINAPI Detour(Args... args) {
        using Function = Result(WINAPI*)(Args...);
        const auto target = reinterpret_cast<Function>(SelectEntryTarget(slot, __builtin_return_address(0)));
        NativeRuntimeScope nativeScope;
        return target(args...);
    }
    static Result WINAPI ProxyDetour(Args... args) {
        using Function = Result(WINAPI*)(Args...);
        const auto target = reinterpret_cast<Function>(slot.replacement.load(std::memory_order_acquire));
        NativeRuntimeScope nativeScope;
        return target(args...);
    }
};

template <size_t Index, typename Result, typename... Args>
EntrySlot EntryForwarder<Index, Result(WINAPI*)(Args...)>::slot;

struct EntryDefinition {
    const char* module;
    const char* name;
    void* detour;
    void* proxyDetour;
    EntrySlot* slot;
};

template <size_t Index, typename Signature>
EntryDefinition Entry(const char* module, const char* name) {
    return {module, name, reinterpret_cast<void*>(&EntryForwarder<Index, Signature>::Detour),
            reinterpret_cast<void*>(&EntryForwarder<Index, Signature>::ProxyDetour),
            &EntryForwarder<Index, Signature>::slot};
}

const std::vector<EntryDefinition>& Definitions() {
    static const std::vector<EntryDefinition> kDefinitions = {
        Entry<0, decltype(&CreateDXGIFactory)>("dxgi.dll", "CreateDXGIFactory"),
        Entry<1, decltype(&CreateDXGIFactory1)>("dxgi.dll", "CreateDXGIFactory1"),
        Entry<2, decltype(&CreateDXGIFactory2)>("dxgi.dll", "CreateDXGIFactory2"),
        Entry<3, decltype(&DXGIGetDebugInterface1)>("dxgi.dll", "DXGIGetDebugInterface1"),
        // The pinned MinGW headers omit this no-argument HRESULT export.
        Entry<4, HRESULT(WINAPI*)()>("dxgi.dll", "DXGIDeclareAdapterRemovalSupport"),
        Entry<5, decltype(&D3D12CreateDevice)>("d3d12.dll", "D3D12CreateDevice"),
        Entry<6, decltype(&D3D12GetDebugInterface)>("d3d12.dll", "D3D12GetDebugInterface"),
        Entry<7, decltype(&D3D12GetInterface)>("d3d12.dll", "D3D12GetInterface"),
        Entry<8, decltype(&D3D12SerializeRootSignature)>("d3d12.dll", "D3D12SerializeRootSignature"),
        Entry<9, decltype(&D3D12SerializeVersionedRootSignature)>("d3d12.dll", "D3D12SerializeVersionedRootSignature"),
        Entry<10, decltype(&D3D12CreateRootSignatureDeserializer)>("d3d12.dll", "D3D12CreateRootSignatureDeserializer"),
        Entry<11, decltype(&D3D12CreateVersionedRootSignatureDeserializer)>(
            "d3d12.dll", "D3D12CreateVersionedRootSignatureDeserializer"),
        Entry<12, decltype(&D3D12EnableExperimentalFeatures)>("d3d12.dll", "D3D12EnableExperimentalFeatures"),
        Entry<13, decltype(&D3D11CreateDevice)>("d3d11.dll", "D3D11CreateDevice"),
        Entry<14, decltype(&D3D11CreateDeviceAndSwapChain)>("d3d11.dll", "D3D11CreateDeviceAndSwapChain"),
        Entry<15, decltype(&D3D11On12CreateDevice)>("d3d11.dll", "D3D11On12CreateDevice"),
        Entry<16, decltype(&D3D10CreateDevice)>("d3d10.dll", "D3D10CreateDevice"),
        Entry<17, decltype(&D3D10CreateDeviceAndSwapChain)>("d3d10.dll", "D3D10CreateDeviceAndSwapChain"),
        Entry<18, decltype(&D3D10CreateDevice1)>("d3d10_1.dll", "D3D10CreateDevice1"),
        Entry<19, decltype(&D3D10CreateDeviceAndSwapChain1)>("d3d10_1.dll", "D3D10CreateDeviceAndSwapChain1"),
    };
    return kDefinitions;
}

void PublishTrampoline(void* trampoline, void* context) {
    static_cast<std::atomic<void*>*>(context)->store(trampoline, std::memory_order_release);
}

bool AddressInModule(const void* address, HMODULE module) {
    HMODULE owner = nullptr;
    return address &&
           GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(address), &owner) &&
           owner == module;
}

}  // namespace

EntryForwardTally InstallEntryForwarders(const char* baseName, HMODULE superseded, HMODULE replacement) {
    EntryForwardTally tally;
    std::vector<InlineHook::PublishedHookSpec> specs;
    std::vector<const EntryDefinition*> definitions;
    for (const EntryDefinition& definition : Definitions()) {
        if (!ce::overlay_compat::detail::EqualsInsensitive(definition.module, baseName)) {
            continue;
        }
        void* entry = reinterpret_cast<void*>(GetProcAddress(superseded, definition.name));
        // An export whose address left the image is another component's export-table patch; its
        // entry is not System32's code to wrap.
        if (!entry || !AddressInModule(entry, superseded)) {
            continue;
        }
        void* target = reinterpret_cast<void*>(GetProcAddress(replacement, definition.name));
        if (!target) {
            if (tally.missingInReplacement++ == 0) {
                tally.firstMissing = definition.name;
            }
            continue;
        }
        // A proxy may export an alias forwarded to the native DLL. There is no override to install
        // for that name: sending the native entry back to itself would recurse indefinitely.
        if (AddressInModule(target, superseded)) {
            ++tally.nativeAliases;
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
        spec.target = target;
        spec.detour = definition.proxyDetour;
        spec.publisherContext = &definition.slot->replacement;
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
    for (size_t i = 0; i < definitions.size(); ++i) {
        const auto& native = specs[2 * i];
        const auto& proxy = specs[2 * i + 1];
        if (native.installed && proxy.installed) {
            ++tally.installed;
            continue;
        }
        // A partial pair remains safe: a missing replacement trampoline selects the native one;
        // a live proxy guard only calls its published predecessor. Never clear a live predecessor.
        if (native.leftToForeignOwner || proxy.leftToForeignOwner) {
            ++tally.leftToForeignOwner;
        } else {
            ++tally.failed;
        }
        HookLogImportant("ModuleReplacement: %s!%s entry not forwarded (%s)", definitions[i]->module,
                         definitions[i]->name,
                         native.leftToForeignOwner || proxy.leftToForeignOwner
                             ? "another component patched it first" : "install failed");
    }
    return tally;
}

}  // namespace ce::module_replacement::detail
