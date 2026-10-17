#pragma once

// Forced-AF facts that belong to D3D11 objects, shared by the wrapper and the raw
// hooks: a pixel shader's sampler/texture plan (from its disassembly) and a shader
// resource view's AF class. Both are immutable for the object and stored on it
// as private data, so no process-global map, lock or pointer-reuse hazard exists.

#include <d3d11.h>

#include <atomic>

#include "hook/overrides/sampler_override_utils.h"
#include "hook/overrides/sampler_override/shader_af_plan.h"

namespace ce::d3d11_af {

// {CEAF1103-5A7D-4A4E-93B8-C3117E6DAF03}
inline const GUID kShaderMetadataGuid = {0xceaf1103, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x03}};
inline constexpr UINT kShaderMetadataMagic = 0x4D464143u;
inline constexpr UINT kShaderMetadataVersion = 3;

struct ShaderMetadata {
    ce::sampler_override::D3D11ShaderSamplerUsage usage = {};
    bool available = false;
    bool disassembleFailed = false;
};

// Owned by the shader through SetPrivateDataInterface; contexts hold acquired references.
struct ShaderMetadataHandle final : IUnknown {
    UINT magic = kShaderMetadataMagic;
    UINT version = kShaderMetadataVersion;
    ShaderMetadata metadata = {};
    ce::sampler_override::D3D11ShaderAFPlan plan;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (riid == IID_IUnknown) {
            AddRef();
            *object = static_cast<IUnknown*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }
    void BuildLookupTables() {
        plan = ce::sampler_override::BuildD3D11ShaderAFPlan(metadata.usage, metadata.available,
                                                             metadata.disassembleFailed);
    }

private:
    std::atomic<ULONG> refs_{1};
};

// At CreatePixelShader: disassembles once and stores the plan on the shader.
void RegisterShaderMetadata(ID3D11PixelShader* shader, const void* bytecode, SIZE_T bytecodeLength);
// Acquired, or null for a shader created before CE attached or not disassemblable.
ShaderMetadataHandle* AcquireShaderMetadata(ID3D11PixelShader* shader);

// The view's AF class, classified on first use and stored on the view.
ce::sampler_override::D3D11ForcedAFResourceDecision ClassifyView(
    ID3D11ShaderResourceView* view, ce::sampler_override::D3D11Texture2DForcedAFInfo* outInfo = nullptr);
// One private-data read, no classification; false when the view was never classified.
bool TryGetCachedViewClass(ID3D11ShaderResourceView* view, ce::sampler_override::D3D11ForcedAFResourceDecision* out);
// At CreateShaderResourceView: classifies off the draw path.
void RegisterViewMetadata(ID3D11ShaderResourceView* view);

}  // namespace ce::d3d11_af
