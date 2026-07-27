/**
 * D3D11 device-context wrapper — shared internals
 *
 * d3d11_context_forced_af.cpp, d3d11_devicecontext_wrap.cpp,
 * d3d11_devicecontext_forward.cpp and d3d11_devicecontext_get.cpp implement one
 * wrapper between them. This header carries what they share.
 *
 * The stage/view constants, the forwarding scope and the two range/hash helpers
 * are defined here rather than in a sibling .cpp on purpose: the x86 hook DLL is
 * built without LTO, so anything on the per-draw or per-bind path must stay
 * inlinable across these translation units.
 */

#pragma once

#include "d3d11_devicecontext_wrap.h"

#include <atomic>
#include <cstdint>
#include "../apis/dx11_hook.h"
#include "../common/sampler_override_utils.h"

// Shader metadata is immutable and owned by the shader COM object. Private data
// avoids a process-global pointer map (and its lock/pointer-reuse hazards) on
// every draw-time sampler decision.
// {CEAF1103-5A7D-4A4E-93B8-C3117E6DAF03}
inline const GUID kWrapperPixelShaderAFMetadataGuid = {
    0xceaf1103, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x03}};
inline constexpr UINT kWrapperPixelShaderAFMetadataMagic = 0x4D464143u;
inline constexpr UINT kWrapperPixelShaderAFMetadataVersion = 2;

struct WrapperPixelShaderAFMetadataHandle final : IUnknown {
    UINT magic = kWrapperPixelShaderAFMetadataMagic;
    UINT version = kWrapperPixelShaderAFMetadataVersion;
    WrapperPixelShaderAFMetadata metadata = {};
    uint32_t candidateSamplerMask = 0;
    uint32_t textureSamplerMasks[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    uint16_t samplerTextureCounts[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    uint8_t samplerTextureSlots[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT][D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] =
        {};

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

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_RefCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = m_RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    void BuildLookupTables() {
        if (!metadata.available) {
            return;
        }
        candidateSamplerMask = ce::sampler_override::D3D11ShaderAFSafeSamplerMaskForAnyTexture(metadata.usage);
        for (UINT samplerSlot = 0; samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++samplerSlot) {
            if ((candidateSamplerMask & (1u << samplerSlot)) == 0) {
                continue;
            }
            for (UINT textureSlot = 0; textureSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++textureSlot) {
                if (!ce::sampler_override::D3D11ShaderSamplerUsesTexture(metadata.usage, samplerSlot, textureSlot)) {
                    continue;
                }
                textureSamplerMasks[textureSlot] |= (1u << samplerSlot);
                const uint16_t index = samplerTextureCounts[samplerSlot]++;
                samplerTextureSlots[samplerSlot][index] = static_cast<uint8_t>(textureSlot);
            }
        }
    }

private:
    std::atomic<ULONG> m_RefCount{1};
};

inline constexpr UINT kWrapperStagePS = 0;
inline constexpr UINT kWrapperStageVS = 1;
inline constexpr UINT kWrapperStageGS = 2;
inline constexpr UINT kWrapperStageHS = 3;
inline constexpr UINT kWrapperStageDS = 4;
inline constexpr UINT kWrapperStageCS = 5;
inline constexpr uint8_t kWrapperAFViewUnknown = 0;
inline constexpr uint8_t kWrapperAFViewIneligible = 1;
inline constexpr uint8_t kWrapperAFViewEligible = 2;

class DX11WrapperForwardingScope {
public:
    DX11WrapperForwardingScope() {
        DX11Hook_BeginWrapperContextForwarding();
    }

    ~DX11WrapperForwardingScope() {
        DX11Hook_EndWrapperContextForwarding();
    }

    DX11WrapperForwardingScope(const DX11WrapperForwardingScope&) = delete;
    DX11WrapperForwardingScope& operator=(const DX11WrapperForwardingScope&) = delete;
};

inline uint32_t WrapperSamplerRangeMask(UINT startSlot, UINT numSamplers) {
    if (startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSamplers == 0) {
        return 0;
    }
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    uint32_t mask = 0;
    for (UINT i = 0; i < actualSamplers; ++i) {
        mask |= (1u << (startSlot + i));
    }
    return mask;
}

inline uint64_t HashWrapperSamplerConfig(const GraphicsConfig& gfx) {
    return ce::sampler_override::HashSamplerOverrideConfig(gfx);
}

// Diagnostic counters shared with the tracking members; defined in
// d3d11_context_forced_af.cpp.
extern std::atomic<int> g_WrapperAFSkipNoSRV;
extern std::atomic<int> g_WrapperAFSkipNoShader;
extern std::atomic<int> g_WrapperAFSkipNoShaderMetadata;

// Defined in d3d11_context_forced_af.cpp. Both return an acquired reference, or
// null when there is none to hand out.
WrapperPixelShaderAFMetadataHandle* AcquireWrapperPixelShaderAFMetadata(ID3D11PixelShader* shader);
ID3D11SamplerState* AcquireWrapperReplacementSampler(ID3D11Device* realDevice, ID3D11SamplerState* original,
                                                     const GraphicsConfig& gfx);
