#include "dx12_sampler_hooks.h"

#include "../common/fg_cost_probe.h"

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../common/dx12_sampler_policy.h"
#include "../common/hook_common.h"
#include "../common/sampler_override_utils.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_hooks.h"
#include "dx12_hook.h"

D3D12CreateDeviceRawPtr oD3D12CreateDeviceRaw = nullptr;
D3D12GetInterfacePtr oD3D12GetInterface = nullptr;
D3D12SerializeRootSignaturePtr oSerializeRootSignature = nullptr;
D3D12SerializeVersionedRootSignaturePtr oSerializeVersionedRootSignature = nullptr;

namespace ce::dx12_sampler_hooks {
namespace {

using CreateSamplerPtr = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_SAMPLER_DESC*,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE);
struct SamplerDesc2Compat {
    D3D12_FILTER Filter;
    D3D12_TEXTURE_ADDRESS_MODE AddressU;
    D3D12_TEXTURE_ADDRESS_MODE AddressV;
    D3D12_TEXTURE_ADDRESS_MODE AddressW;
    FLOAT MipLODBias;
    UINT MaxAnisotropy;
    D3D12_COMPARISON_FUNC ComparisonFunc;
    union {
        FLOAT FloatBorderColor[4];
        UINT UintBorderColor[4];
    };
    FLOAT MinLOD;
    FLOAT MaxLOD;
    UINT Flags;
};
struct StaticSamplerDesc1Compat {
    D3D12_STATIC_SAMPLER_DESC base;
    UINT Flags;
};
struct RootSignatureDesc2Compat {
    UINT NumParameters;
    const D3D12_ROOT_PARAMETER1* pParameters;
    UINT NumStaticSamplers;
    const StaticSamplerDesc1Compat* pStaticSamplers;
    D3D12_ROOT_SIGNATURE_FLAGS Flags;
};
using CreateSampler2Ptr = void(STDMETHODCALLTYPE*)(IUnknown*, const SamplerDesc2Compat*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateRootSignaturePtr = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);

struct DeviceOriginals {
    CreateSamplerPtr createSampler = nullptr;
    CreateSampler2Ptr createSampler2 = nullptr;
    CreateRootSignaturePtr createRootSignature = nullptr;
};

struct DecisionCounters {
    std::atomic<uint64_t> observed{0};
    std::atomic<uint64_t> modified{0};
    std::array<std::atomic<uint64_t>, 10> decisions{};
};

std::mutex g_deviceHookMutex;
std::unordered_map<void**, DeviceOriginals> g_deviceOriginals;
using FactoryCreateDevicePtr = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
std::unordered_map<void**, FactoryCreateDevicePtr> g_factoryOriginals;
DecisionCounters g_dynamicCounters;
DecisionCounters g_staticCounters;
std::atomic<uint64_t> g_deviceCreateCalls{0};
std::atomic<uint64_t> g_deviceHookSuccesses{0};
std::atomic<uint64_t> g_rootSignatureCalls{0};
std::atomic<uint64_t> g_firstConfigHash{0};
std::atomic<bool> g_configChangeLogged{false};
std::mutex g_fingerprintMutex;
std::unordered_set<uint64_t> g_loggedFingerprints;

constexpr size_t DecisionIndex(ce::dx12_sampler_policy::Decision decision) {
    return static_cast<size_t>(decision);
}

void LogConfigOnce() {
    const GraphicsConfig& gfx = GetActiveGraphicsConfig();
    const uint64_t configHash = ce::sampler_override::HashSamplerOverrideConfig(gfx);
    uint64_t expected = 0;
    if (g_firstConfigHash.compare_exchange_strong(expected, configHash, std::memory_order_acq_rel)) {
        HookLogImportant(
            "DX12 sampler overrides: creation-time policy configured (policy=%s af=%s mip=%s mipBias=%s "
            "mipMode=%s clamp=%d)",
            gfx.samplerOverrideMode.c_str(), gfx.anisotropicFiltering.c_str(), gfx.mipMapping.c_str(),
            gfx.mipBias.c_str(), gfx.mipBiasMode.c_str(), gfx.forceMipBiasClamp ? 1 : 0);
    } else if (expected != configHash && !g_configChangeLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "DX12 AF: sampler-affecting configuration changed after descriptor creation began "
            "(firstHash=0x%llX currentHash=0x%llX); existing immutable samplers/root signatures require restart",
            static_cast<unsigned long long>(expected), static_cast<unsigned long long>(configHash));
    }
}

template <typename Desc>
void RecordDecision(DecisionCounters& counters, const char* source, const Desc& original,
                    const ce::dx12_sampler_policy::Result& result) {
    LogConfigOnce();
    counters.observed.fetch_add(1, std::memory_order_relaxed);
    if (result.Modified()) {
        counters.modified.fetch_add(1, std::memory_order_relaxed);
    }
    const size_t index = DecisionIndex(result.decision);
    if (index < counters.decisions.size()) {
        counters.decisions[index].fetch_add(1, std::memory_order_relaxed);
    }

    const uint64_t fingerprint = ce::dx12_sampler_policy::Fingerprint(original);
    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lock(g_fingerprintMutex);
        if (g_loggedFingerprints.size() < 16) {
            shouldLog = g_loggedFingerprints.insert(fingerprint).second;
        }
    }
    if (shouldLog) {
        HookLog(
            "DX12 AF: %s sampler fingerprint=0x%llX decision=%s afChanged=%d mipChanged=%d biasChanged=%d "
            "filter=0x%X address=%u/%u/%u aniso=%u lod=%.3f..%.3f bias=%.3f",
            source, static_cast<unsigned long long>(fingerprint),
            ce::dx12_sampler_policy::DecisionName(result.decision), result.anisotropyModified ? 1 : 0,
            result.mipMappingModified ? 1 : 0, result.mipBiasModified ? 1 : 0, static_cast<unsigned>(original.Filter),
            static_cast<unsigned>(original.AddressU), static_cast<unsigned>(original.AddressV),
            static_cast<unsigned>(original.AddressW), original.MaxAnisotropy, original.MinLOD, original.MaxLOD,
            original.MipLODBias);
    }
}

ce::dx12_sampler_policy::Result ApplyDynamicSampler(D3D12_SAMPLER_DESC& desc, const char* source) {
    const D3D12_SAMPLER_DESC original = desc;
    const auto result = ce::dx12_sampler_policy::Apply(desc, GetActiveGraphicsConfig());
    RecordDecision(g_dynamicCounters, source, original, result);
    return result;
}

ce::dx12_sampler_policy::Result ApplyStaticSampler(D3D12_STATIC_SAMPLER_DESC& desc, const char* source) {
    const D3D12_STATIC_SAMPLER_DESC original = desc;
    const auto result = ce::dx12_sampler_policy::Apply(desc, GetActiveGraphicsConfig());
    RecordDecision(g_staticCounters, source, original, result);
    return result;
}

DeviceOriginals FindOriginals(ID3D12Device* device) {
    if (!device) {
        return {};
    }
    void** vtable = *reinterpret_cast<void***>(device);
    std::lock_guard<std::mutex> lock(g_deviceHookMutex);
    const auto it = g_deviceOriginals.find(vtable);
    return it == g_deviceOriginals.end() ? DeviceOriginals{} : it->second;
}

HRESULT STDMETHODCALLTYPE DetourFactoryCreateDevice(IUnknown* factory, IUnknown* adapter,
                                                    D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid, void** device) {
    FactoryCreateDevicePtr original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_deviceHookMutex);
        const auto it = g_factoryOriginals.find(*reinterpret_cast<void***>(factory));
        if (it != g_factoryOriginals.end())
            original = it->second;
    }
    if (!original)
        return E_FAIL;
    const HRESULT hr = original(factory, adapter, minimumFeatureLevel, riid, device);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && device && *device) {
        ID3D12Device* baseDevice = nullptr;
        auto* unknown = reinterpret_cast<IUnknown*>(*device);
        if (SUCCEEDED(unknown->QueryInterface(IID_ID3D12Device, reinterpret_cast<void**>(&baseDevice))) && baseDevice) {
            DX12_HookDeviceVTable(baseDevice);
            baseDevice->Release();
        }
    }
    return hr;
}

void HookDeviceFactory(IUnknown* factory) {
    if (!factory)
        return;
    void** vtable = *reinterpret_cast<void***>(factory);
    if (!vtable || !vtable[9])
        return;
    std::lock_guard<std::mutex> lock(g_deviceHookMutex);
    if (g_factoryOriginals.find(vtable) != g_factoryOriginals.end())
        return;
    FactoryCreateDevicePtr original = nullptr;
    const VTableHook::Status status = VTableHook::Create(
        reinterpret_cast<void*>(&vtable[9]), reinterpret_cast<void*>(&DetourFactoryCreateDevice), reinterpret_cast<void**>(&original));
    if (status == VTableHook::Success && original) {
        g_factoryOriginals.emplace(vtable, original);
        HookLogImportant("DX12 AF: ID3D12DeviceFactory::CreateDevice hook ready factory=%p vtable=%p", factory, vtable);
    } else {
        HookLogImportant("DX12 AF: ID3D12DeviceFactory hook failed factory=%p status=%s", factory,
                         VTableHook::StatusToString(status));
    }
}

bool ModifyStaticSamplers(D3D12_ROOT_SIGNATURE_DESC& desc, std::vector<D3D12_STATIC_SAMPLER_DESC>& samplers,
                          const char* source) {
    if (desc.NumStaticSamplers == 0 || !desc.pStaticSamplers) {
        return false;
    }
    samplers.assign(desc.pStaticSamplers, desc.pStaticSamplers + desc.NumStaticSamplers);
    bool modified = false;
    for (auto& sampler : samplers) {
        modified = ApplyStaticSampler(sampler, source).Modified() || modified;
    }
    if (modified) {
        desc.pStaticSamplers = samplers.data();
    }
    return modified;
}

bool ModifyStaticSamplers(D3D12_ROOT_SIGNATURE_DESC1& desc, std::vector<D3D12_STATIC_SAMPLER_DESC>& samplers,
                          const char* source) {
    if (desc.NumStaticSamplers == 0 || !desc.pStaticSamplers) {
        return false;
    }
    samplers.assign(desc.pStaticSamplers, desc.pStaticSamplers + desc.NumStaticSamplers);
    bool modified = false;
    for (auto& sampler : samplers) {
        modified = ApplyStaticSampler(sampler, source).Modified() || modified;
    }
    if (modified) {
        desc.pStaticSamplers = samplers.data();
    }
    return modified;
}

bool ModifyStaticSamplers(RootSignatureDesc2Compat& desc, std::vector<StaticSamplerDesc1Compat>& samplers,
                          const char* source) {
    if (desc.NumStaticSamplers == 0 || !desc.pStaticSamplers)
        return false;
    samplers.assign(desc.pStaticSamplers, desc.pStaticSamplers + desc.NumStaticSamplers);
    bool modified = false;
    for (auto& sampler : samplers) {
        // NON_NORMALIZED_COORDINATES has sampling restrictions that ordinary
        // material overrides must never rewrite.
        if ((sampler.Flags & 0x2u) == 0)
            modified = ApplyStaticSampler(sampler.base, source).Modified() || modified;
    }
    if (modified)
        desc.pStaticSamplers = samplers.data();
    return modified;
}

bool RewriteRootSignatureBlob(const void* blob, SIZE_T blobSize, ID3DBlob** rewrittenBlob) {
    if (!blob || blobSize == 0 || !rewrittenBlob || !oSerializeVersionedRootSignature) {
        return false;
    }
    *rewrittenBlob = nullptr;

    ID3D12VersionedRootSignatureDeserializer* deserializer = nullptr;
    const HRESULT deserializeHr = D3D12CreateVersionedRootSignatureDeserializer(
        blob, blobSize, IID_ID3D12VersionedRootSignatureDeserializer, reinterpret_cast<void**>(&deserializer));
    if (FAILED(deserializeHr) || !deserializer) {
        static std::atomic<uint32_t> failureLogs{0};
        if (failureLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
            HookLog("DX12 AF: root-signature blob deserialization failed hr=0x%08X size=%zu; passing through",
                    static_cast<unsigned>(deserializeHr), static_cast<size_t>(blobSize));
        }
        return false;
    }

    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* original = deserializer->GetUnconvertedRootSignatureDesc();
    if (!original) {
        deserializer->Release();
        return false;
    }

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC modified = *original;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    bool anyModified = false;
    if (static_cast<unsigned>(modified.Version) == 3u) {
        auto root12 = *reinterpret_cast<const RootSignatureDesc2Compat*>(&original->Desc_1_0);
        std::vector<StaticSamplerDesc1Compat> samplers12;
        if (ModifyStaticSamplers(root12, samplers12, "precompiled-v1.2")) {
            struct VersionedRootSignatureDesc12Compat {
                D3D_ROOT_SIGNATURE_VERSION Version;
                RootSignatureDesc2Compat Desc_1_2;
            } root = {static_cast<D3D_ROOT_SIGNATURE_VERSION>(3), root12};
            ID3DBlob* errors = nullptr;
            const HRESULT serializeHr = oSerializeVersionedRootSignature(
                reinterpret_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*>(&root), rewrittenBlob, &errors);
            if (errors)
                errors->Release();
            deserializer->Release();
            if (SUCCEEDED(serializeHr) && *rewrittenBlob)
                return true;
            if (*rewrittenBlob) {
                (*rewrittenBlob)->Release();
                *rewrittenBlob = nullptr;
            }
            static std::atomic<uint32_t> failureLogs{0};
            if (failureLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
                HookLogImportant("DX12 AF: root-signature 1.2 reserialization failed hr=0x%08X; passing through",
                                 static_cast<unsigned>(serializeHr));
            }
            return false;
        }
    } else if (modified.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
        anyModified = ModifyStaticSamplers(modified.Desc_1_0, samplers, "precompiled-v1.0");
    } else if (modified.Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        anyModified = ModifyStaticSamplers(modified.Desc_1_1, samplers, "precompiled-v1.1");
    }

    if (anyModified) {
        ID3DBlob* errors = nullptr;
        const HRESULT serializeHr = oSerializeVersionedRootSignature(&modified, rewrittenBlob, &errors);
        if (errors) {
            errors->Release();
        }
        if (FAILED(serializeHr) || !*rewrittenBlob) {
            static std::atomic<uint32_t> failureLogs{0};
            if (failureLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
                HookLogImportant("DX12 AF: root-signature reserialization failed hr=0x%08X version=%u; passing through",
                                 static_cast<unsigned>(serializeHr), static_cast<unsigned>(modified.Version));
            }
            if (*rewrittenBlob) {
                (*rewrittenBlob)->Release();
                *rewrittenBlob = nullptr;
            }
            anyModified = false;
        }
    }

    deserializer->Release();
    return anyModified;
}

void STDMETHODCALLTYPE DetourCreateSampler(ID3D12Device* device, const D3D12_SAMPLER_DESC* desc,
                                           D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const DeviceOriginals originals = FindOriginals(device);
    if (!originals.createSampler) {
        HookLogImportant("DX12 AF: CreateSampler detour has no per-vtable original for device=%p", device);
        return;
    }
    if (!desc) {
        originals.createSampler(device, desc, destination);
        return;
    }
    if (HookIsShuttingDown()) {
        originals.createSampler(device, desc, destination);
        return;
    }

    D3D12_SAMPLER_DESC modified = *desc;
    ApplyDynamicSampler(modified, "dynamic");
    originals.createSampler(device, &modified, destination);
}

void STDMETHODCALLTYPE DetourCreateSampler2(IUnknown* device, const SamplerDesc2Compat* desc,
                                            D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const DeviceOriginals originals = FindOriginals(reinterpret_cast<ID3D12Device*>(device));
    if (!originals.createSampler2) {
        HookLogImportant("DX12 AF: CreateSampler2 detour has no per-vtable original for device=%p", device);
        return;
    }
    if (HookIsShuttingDown() || !desc || (desc->Flags & 0x2u) != 0) {
        originals.createSampler2(device, desc, destination);
        return;
    }
    SamplerDesc2Compat modified = *desc;
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_SAMPLER_DESC ordinary = {};
    static_assert(sizeof(ordinary) + sizeof(UINT) == sizeof(modified));
    std::memcpy(&ordinary, &modified, sizeof(ordinary));
    ApplyDynamicSampler(ordinary, "dynamic-v2");
    std::memcpy(&modified, &ordinary, sizeof(ordinary));
    originals.createSampler2(device, &modified, destination);
}

HRESULT STDMETHODCALLTYPE DetourCreateRootSignature(ID3D12Device* device, UINT nodeMask, const void* blob,
                                                    SIZE_T blobSize, REFIID riid, void** rootSignature) {
    const DeviceOriginals originals = FindOriginals(device);
    if (!originals.createRootSignature) {
        HookLogImportant("DX12 AF: CreateRootSignature detour has no per-vtable original for device=%p", device);
        return E_FAIL;
    }
    if (HookIsShuttingDown())
        return originals.createRootSignature(device, nodeMask, blob, blobSize, riid, rootSignature);

    g_rootSignatureCalls.fetch_add(1, std::memory_order_relaxed);
    ID3DBlob* rewritten = nullptr;
    const bool modified = RewriteRootSignatureBlob(blob, blobSize, &rewritten);
    const void* forwardedBlob = modified ? rewritten->GetBufferPointer() : blob;
    const SIZE_T forwardedSize = modified ? rewritten->GetBufferSize() : blobSize;
    const HRESULT hr =
        originals.createRootSignature(device, nodeMask, forwardedBlob, forwardedSize, riid, rootSignature);
    if (rewritten) {
        rewritten->Release();
    }
    return hr;
}

}  // namespace

bool HookDevice(ID3D12Device* device) {
    if (HookIsShuttingDown() || !device) {
        return false;
    }
    if (ce::fg_cost_probe::Active(ce::fg_cost_probe::kSamplerDeviceHooksOff)) {
        return false;
    }
    LogConfigOnce();

    void** vtable = *reinterpret_cast<void***>(device);
    if (!vtable || !vtable[16] || !vtable[22]) {
        HookLogImportant("DX12 AF: device vtable lacks CreateRootSignature/CreateSampler slots (device=%p vtable=%p)",
                         device, vtable);
        return false;
    }

    std::lock_guard<std::mutex> lock(g_deviceHookMutex);
    auto [it, inserted] = g_deviceOriginals.try_emplace(vtable);
    DeviceOriginals& originals = it->second;

    bool success = true;
    if (vtable[16] != reinterpret_cast<void*>(&DetourCreateRootSignature)) {
        CreateRootSignaturePtr original = nullptr;
        const VTableHook::Status status = VTableHook::Create(
            reinterpret_cast<void*>(&vtable[16]), reinterpret_cast<void*>(&DetourCreateRootSignature), reinterpret_cast<void**>(&original));
        if (status == VTableHook::Success && original) {
            originals.createRootSignature = original;
        } else {
            HookLogImportant("DX12 AF: CreateRootSignature hook failed device=%p vtable=%p status=%s", device, vtable,
                             VTableHook::StatusToString(status));
            success = false;
        }
    }

    if (vtable[22] != reinterpret_cast<void*>(&DetourCreateSampler)) {
        CreateSamplerPtr original = nullptr;
        const VTableHook::Status status = VTableHook::Create(reinterpret_cast<void*>(&vtable[22]), reinterpret_cast<void*>(&DetourCreateSampler),
                                                             reinterpret_cast<void**>(&original));
        if (status == VTableHook::Success && original) {
            originals.createSampler = original;
        } else {
            HookLogImportant("DX12 AF: CreateSampler hook failed device=%p vtable=%p status=%s", device, vtable,
                             VTableHook::StatusToString(status));
            success = false;
        }
    }

    if (!inserted && (!originals.createSampler || !originals.createRootSignature)) {
        success = false;
    }
    if (success) {
        g_deviceHookSuccesses.fetch_add(1, std::memory_order_relaxed);
        HookLogImportant("DX12 AF: actual device sampler/root-signature hooks ready device=%p vtable=%p", device,
                         vtable);
    }

    static const GUID iidDevice11 = {0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
    IUnknown* device11 = nullptr;
    if (SUCCEEDED(device->QueryInterface(iidDevice11, reinterpret_cast<void**>(&device11))) && device11) {
        void** vtable11 = *reinterpret_cast<void***>(device11);
        if (vtable11 && vtable11[84] != reinterpret_cast<void*>(&DetourCreateSampler2)) {
            CreateSampler2Ptr original = nullptr;
            const VTableHook::Status status = VTableHook::Create(
                reinterpret_cast<void*>(&vtable11[84]), reinterpret_cast<void*>(&DetourCreateSampler2), reinterpret_cast<void**>(&original));
            if (status == VTableHook::Success && original) {
                g_deviceOriginals[vtable11].createSampler2 = original;
                HookLogImportant("DX12 AF: CreateSampler2 hook ready device=%p vtable=%p", device11, vtable11);
            } else {
                HookLogImportant("DX12 AF: CreateSampler2 hook failed device=%p status=%s", device11,
                                 VTableHook::StatusToString(status));
                success = false;
            }
        }
        device11->Release();
    }
    return success;
}

void LogSummary(const char* reason) {
    const uint64_t dynamicObserved = g_dynamicCounters.observed.load(std::memory_order_relaxed);
    const uint64_t dynamicModified = g_dynamicCounters.modified.load(std::memory_order_relaxed);
    const uint64_t staticObserved = g_staticCounters.observed.load(std::memory_order_relaxed);
    const uint64_t staticModified = g_staticCounters.modified.load(std::memory_order_relaxed);
    HookLogImportant(
        "DX12 AF: %s summary deviceCreates=%llu deviceHooks=%llu rootSignatures=%llu "
        "dynamic(observed=%llu modified=%llu) static(observed=%llu modified=%llu)",
        reason ? reason : "runtime", static_cast<unsigned long long>(g_deviceCreateCalls.load()),
        static_cast<unsigned long long>(g_deviceHookSuccesses.load()),
        static_cast<unsigned long long>(g_rootSignatureCalls.load()), static_cast<unsigned long long>(dynamicObserved),
        static_cast<unsigned long long>(dynamicModified), static_cast<unsigned long long>(staticObserved),
        static_cast<unsigned long long>(staticModified));

    for (size_t index = 0; index < g_dynamicCounters.decisions.size(); ++index) {
        const uint64_t dynamicCount = g_dynamicCounters.decisions[index].load(std::memory_order_relaxed);
        const uint64_t staticCount = g_staticCounters.decisions[index].load(std::memory_order_relaxed);
        if (dynamicCount || staticCount) {
            HookLog("DX12 AF: decision summary reason=%s dynamic=%llu static=%llu",
                    ce::dx12_sampler_policy::DecisionName(static_cast<ce::dx12_sampler_policy::Decision>(index)),
                    static_cast<unsigned long long>(dynamicCount), static_cast<unsigned long long>(staticCount));
        }
    }

    const GraphicsConfig& gfx = GetActiveGraphicsConfig();
    if (ce::sampler_override::IsAnisotropicOverrideEnabled(gfx)) {
        if (dynamicObserved + staticObserved == 0) {
            HookLogImportant(
                "DX12 AF: WARNING - forced AF was enabled but no dynamic or static sampler descriptors were observed; "
                "the device/export hook was late, bypassed, or all objects were created before injection");
        } else if (dynamicModified + staticModified == 0) {
            HookLogImportant(
                "DX12 AF: forced AF observed sampler descriptors but modified none; decision counters distinguish "
                "already-compliant descriptors from intentionally protected non-material sampler classes");
        }
    }
}

}  // namespace ce::dx12_sampler_hooks

extern "C" BOOL WINAPI ApplyDX12SamplerOverridesCallback(D3D12_SAMPLER_DESC* desc) {
    if (HookIsShuttingDown() || !desc) {
        return FALSE;
    }
    return ce::dx12_sampler_hooks::ApplyDynamicSampler(*desc, "wrapper-dynamic").Modified() ? TRUE : FALSE;
}

HRESULT WINAPI DetourD3D12CreateDeviceRaw(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid,
                                          void** device) {
    if (!oD3D12CreateDeviceRaw) {
        return E_FAIL;
    }
    const HRESULT hr = oD3D12CreateDeviceRaw(adapter, minimumFeatureLevel, riid, device);
    if (FAILED(hr) || HookIsShuttingDown() || !device || !*device) {
        return hr;
    }

    ce::dx12_sampler_hooks::g_deviceCreateCalls.fetch_add(1, std::memory_order_relaxed);
    MarkD3D12DeviceCreated();
    ID3D12Device* baseDevice = nullptr;
    auto* unknown = reinterpret_cast<IUnknown*>(*device);
    if (SUCCEEDED(unknown->QueryInterface(IID_ID3D12Device, reinterpret_cast<void**>(&baseDevice))) && baseDevice) {
        DX12_HookDeviceVTable(baseDevice);
        baseDevice->Release();
    } else {
        HookLogImportant("DX12 AF: D3D12CreateDevice returned interface without ID3D12Device base (riid=%p)", &riid);
    }
    if (g_dx12HookInstance) {
        g_dx12HookInstance->EnsurePresentHooks();
    }
    return hr;
}

HRESULT WINAPI DetourD3D12GetInterface(REFCLSID clsid, REFIID riid, void** object) {
    if (!oD3D12GetInterface)
        return E_FAIL;
    const HRESULT hr = oD3D12GetInterface(clsid, riid, object);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && object && *object) {
        static const GUID iidDeviceFactory = {
            0x61f307d3, 0xd34e, 0x4e7c, {0x83, 0x74, 0x3b, 0xa4, 0xde, 0x23, 0xcc, 0xcb}};
        IUnknown* factory = nullptr;
        auto* unknown = reinterpret_cast<IUnknown*>(*object);
        if (SUCCEEDED(unknown->QueryInterface(iidDeviceFactory, reinterpret_cast<void**>(&factory))) && factory) {
            ce::dx12_sampler_hooks::HookDeviceFactory(factory);
            factory->Release();
        }
    }
    return hr;
}

HRESULT WINAPI DetourSerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* rootSignature,
                                            D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob, ID3DBlob** errorBlob) {
    if (!oSerializeRootSignature) {
        return E_FAIL;
    }
    // CreateRootSignature is the single mutation boundary. Rewriting here as well
    // would apply non-idempotent offset/base bias twice when the resulting blob is
    // passed through the device hook.
    return oSerializeRootSignature(rootSignature, version, blob, errorBlob);
}

HRESULT WINAPI DetourSerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* rootSignature,
                                                     ID3DBlob** blob, ID3DBlob** errorBlob) {
    if (!oSerializeVersionedRootSignature) {
        return E_FAIL;
    }
    return oSerializeVersionedRootSignature(rootSignature, blob, errorBlob);
}
