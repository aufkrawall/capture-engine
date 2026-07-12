#include "dx12_sampler_hooks.h"

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
D3D12SerializeRootSignaturePtr oSerializeRootSignature = nullptr;
D3D12SerializeVersionedRootSignaturePtr oSerializeVersionedRootSignature = nullptr;

namespace ce::dx12_sampler_hooks {
namespace {

using CreateSamplerPtr = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_SAMPLER_DESC*,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateRootSignaturePtr = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);

struct DeviceOriginals {
    CreateSamplerPtr createSampler = nullptr;
    CreateRootSignaturePtr createRootSignature = nullptr;
};

struct DecisionCounters {
    std::atomic<uint64_t> observed{0};
    std::atomic<uint64_t> modified{0};
    std::array<std::atomic<uint64_t>, 10> decisions{};
};

std::mutex g_deviceHookMutex;
std::unordered_map<void**, DeviceOriginals> g_deviceOriginals;
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
        HookLogImportant("DX12 AF: creation-time sampler policy configured (af=%s mipBias=%s mipMode=%s clamp=%d)",
                         gfx.anisotropicFiltering.c_str(), gfx.mipBias.c_str(), gfx.mipBiasMode.c_str(),
                         gfx.forceMipBiasClamp ? 1 : 0);
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
        HookLog("DX12 AF: %s sampler fingerprint=0x%llX decision=%s afChanged=%d biasChanged=%d "
                "filter=0x%X address=%u/%u/%u aniso=%u lod=%.3f..%.3f bias=%.3f",
                source, static_cast<unsigned long long>(fingerprint),
                ce::dx12_sampler_policy::DecisionName(result.decision), result.anisotropyModified ? 1 : 0,
                result.mipBiasModified ? 1 : 0, static_cast<unsigned>(original.Filter),
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
    if (modified.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
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
                HookLogImportant(
                    "DX12 AF: root-signature reserialization failed hr=0x%08X version=%u; passing through",
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

    D3D12_SAMPLER_DESC modified = *desc;
    ApplyDynamicSampler(modified, "dynamic");
    originals.createSampler(device, &modified, destination);
}

HRESULT STDMETHODCALLTYPE DetourCreateRootSignature(ID3D12Device* device, UINT nodeMask, const void* blob,
                                                     SIZE_T blobSize, REFIID riid, void** rootSignature) {
    const DeviceOriginals originals = FindOriginals(device);
    if (!originals.createRootSignature) {
        HookLogImportant("DX12 AF: CreateRootSignature detour has no per-vtable original for device=%p", device);
        return E_FAIL;
    }

    g_rootSignatureCalls.fetch_add(1, std::memory_order_relaxed);
    ID3DBlob* rewritten = nullptr;
    const bool modified = RewriteRootSignatureBlob(blob, blobSize, &rewritten);
    const void* forwardedBlob = modified ? rewritten->GetBufferPointer() : blob;
    const SIZE_T forwardedSize = modified ? rewritten->GetBufferSize() : blobSize;
    const HRESULT hr = originals.createRootSignature(device, nodeMask, forwardedBlob, forwardedSize, riid, rootSignature);
    if (rewritten) {
        rewritten->Release();
    }
    return hr;
}

}  // namespace

bool HookDevice(ID3D12Device* device) {
    if (!device) {
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
        const VTableHook::Status status = VTableHook::Create(&vtable[16], reinterpret_cast<void*>(&DetourCreateRootSignature),
                                                             reinterpret_cast<void**>(&original));
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
        const VTableHook::Status status = VTableHook::Create(&vtable[22], reinterpret_cast<void*>(&DetourCreateSampler),
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
    if (!desc) {
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
    if (FAILED(hr) || !device || !*device) {
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

HRESULT WINAPI DetourSerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* rootSignature,
                                             D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob,
                                             ID3DBlob** errorBlob) {
    if (!oSerializeRootSignature) {
        return E_FAIL;
    }
    if (!rootSignature || rootSignature->NumStaticSamplers == 0 || !rootSignature->pStaticSamplers) {
        return oSerializeRootSignature(rootSignature, version, blob, errorBlob);
    }

    D3D12_ROOT_SIGNATURE_DESC modified = *rootSignature;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    if (ce::dx12_sampler_hooks::ModifyStaticSamplers(modified, samplers, "serialize-v1.0")) {
        return oSerializeRootSignature(&modified, version, blob, errorBlob);
    }
    return oSerializeRootSignature(rootSignature, version, blob, errorBlob);
}

HRESULT WINAPI DetourSerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* rootSignature,
                                                      ID3DBlob** blob, ID3DBlob** errorBlob) {
    if (!oSerializeVersionedRootSignature) {
        return E_FAIL;
    }
    if (!rootSignature) {
        return oSerializeVersionedRootSignature(rootSignature, blob, errorBlob);
    }

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC modified = *rootSignature;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    bool anyModified = false;
    if (modified.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
        anyModified = ce::dx12_sampler_hooks::ModifyStaticSamplers(modified.Desc_1_0, samplers, "serialize-v1.0");
    } else if (modified.Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        anyModified = ce::dx12_sampler_hooks::ModifyStaticSamplers(modified.Desc_1_1, samplers, "serialize-v1.1");
    }
    return oSerializeVersionedRootSignature(anyModified ? &modified : rootSignature, blob, errorBlob);
}
