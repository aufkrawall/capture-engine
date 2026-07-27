/**
 * D3D11 Device Context Wrapper Implementation
 *
 * Object lifetime and the forced-AF tracking state that the per-draw path
 * reads. The COM/forwarding surface is split across
 * d3d11_devicecontext_forward.cpp and d3d11_devicecontext_get.cpp; the
 * resource-metadata machinery lives in d3d11_context_forced_af.cpp.
 */

#include "d3d11_devicecontext_wrap.h"
#include "d3d11_devicecontext_wrap_internal.h"

#include <d3dcompiler.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include "../apis/dx11_hook.h"
#include "../common/sampler_override_utils.h"
#include "d3d11_device_wrap.h"
#include "hook_common.h"

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapD3D11DeviceContext::CWrapD3D11DeviceContext(ID3D11DeviceContext* pReal, CWrapD3D11Device* pDevice)
    : m_pReal(pReal),
      m_pReal1(nullptr),
      m_pReal2(nullptr),
      m_pReal3(nullptr),
      m_pReal4(nullptr),
      m_pAFRealDevice(nullptr),
      m_pDevice(pDevice),
      m_RefCount(1),
      m_Version(0),
      m_CurrentPixelShader(nullptr),
      m_CurrentPixelShaderAFMetadata(nullptr),
      m_CurrentPixelAFCandidateMask(0),
      m_PixelTrackedSamplerMask(0),
      m_PixelSamplerDirtyMask(0),
      m_PixelForcedSamplerMask(0),
      m_LastSamplerConfigHash(0),
      m_LastGraphicsConfigVersion(0xFFFFFFFFu),
      m_AFGraphicsConfig(),
      m_AFEnabled(false) {
    std::memset(m_TrackedSRVs, 0, sizeof(m_TrackedSRVs));
    std::memset(m_TrackedSamplers, 0, sizeof(m_TrackedSamplers));
    std::memset(m_RealSamplers, 0, sizeof(m_RealSamplers));
    std::memset(m_PixelSRVAFState, 0, sizeof(m_PixelSRVAFState));
    std::memset(m_ForcedSamplerVariants, 0, sizeof(m_ForcedSamplerVariants));
    std::memset(m_ForcedSamplerVariantResolved, 0, sizeof(m_ForcedSamplerVariantResolved));
    if (pReal) {
        pReal->AddRef();
        pReal->GetDevice(&m_pAFRealDevice);
        PromoteInterfaces();
    }
    if (m_pDevice) {
        m_pDevice->AddRef();
    }
    WrapperLog("D3D11 Context Wrapper: Created (real=%p, version=%d, ctx1=%d ctx2=%d ctx3=%d ctx4=%d)", pReal,
               m_Version, m_pReal1 != nullptr, m_pReal2 != nullptr, m_pReal3 != nullptr, m_pReal4 != nullptr);
}

CWrapD3D11DeviceContext::~CWrapD3D11DeviceContext() {
    WrapperLog("D3D11 Context Wrapper: Destroyed");
    ClearForcedAFTracking();
    if (m_pReal4)
        m_pReal4->Release();
    if (m_pReal3)
        m_pReal3->Release();
    if (m_pReal2)
        m_pReal2->Release();
    if (m_pReal1)
        m_pReal1->Release();
    if (m_pAFRealDevice)
        m_pAFRealDevice->Release();
    if (m_pDevice)
        m_pDevice->Release();
    if (m_pReal)
        m_pReal->Release();
}

void CWrapD3D11DeviceContext::InvalidateDeviceWrapper() {
    if (m_pDevice) {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
}

void CWrapD3D11DeviceContext::PromoteInterfaces() {
    if (!m_pReal)
        return;

    DX11Hook_BeginInternalIdentityProbe();
    // Cache every inherited context interface independently. A modern context may
    // support ID3D11DeviceContext4 while callers still use Context1 methods like
    // ClearView; stopping at the highest interface would leave those forwards null.
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) {
        m_Version = 1;
    }
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2)))) {
        m_Version = 2;
    }
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
        m_Version = 3;
    }
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
        m_Version = 4;
    }
    DX11Hook_EndInternalIdentityProbe();
}

void CWrapD3D11DeviceContext::ClearForcedAFTracking() {
    ClearForcedAFSamplerVariants();
    for (UINT stage = 0; stage < 6; ++stage) {
        for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
            if (m_TrackedSRVs[stage][slot]) {
                m_TrackedSRVs[stage][slot]->Release();
                m_TrackedSRVs[stage][slot] = nullptr;
            }
        }
        for (UINT slot = 0; slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++slot) {
            if (m_TrackedSamplers[stage][slot]) {
                m_TrackedSamplers[stage][slot]->Release();
                m_TrackedSamplers[stage][slot] = nullptr;
            }
            if (m_RealSamplers[stage][slot]) {
                m_RealSamplers[stage][slot]->Release();
                m_RealSamplers[stage][slot] = nullptr;
            }
        }
    }
    if (m_CurrentPixelShader) {
        m_CurrentPixelShader->Release();
        m_CurrentPixelShader = nullptr;
    }
    if (m_CurrentPixelShaderAFMetadata) {
        m_CurrentPixelShaderAFMetadata->Release();
        m_CurrentPixelShaderAFMetadata = nullptr;
    }
    m_PixelSamplerDirtyMask = 0;
    m_PixelTrackedSamplerMask = 0;
    m_PixelForcedSamplerMask = 0;
    m_CurrentPixelAFCandidateMask = 0;
    m_LastSamplerConfigHash = 0;
    m_LastGraphicsConfigVersion = 0xFFFFFFFFu;
    m_AFGraphicsConfig = {};
    m_AFEnabled = false;
    std::memset(m_PixelSRVAFState, 0, sizeof(m_PixelSRVAFState));
}

void CWrapD3D11DeviceContext::ClearForcedAFSamplerVariants() {
    for (ID3D11SamplerState*& variant : m_ForcedSamplerVariants) {
        if (variant) {
            variant->Release();
            variant = nullptr;
        }
    }
    std::memset(m_ForcedSamplerVariantResolved, 0, sizeof(m_ForcedSamplerVariantResolved));
}

void CWrapD3D11DeviceContext::RefreshForcedAFConfig() {
    const uint32_t configVersion = GetActiveGraphicsConfigVersion();
    if (configVersion == m_LastGraphicsConfigVersion && m_LastSamplerConfigHash != 0) {
        return;
    }

    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    const uint64_t configHash = HashWrapperSamplerConfig(gfx);
    if (configHash != m_LastSamplerConfigHash) {
        ClearForcedAFSamplerVariants();
        m_PixelSamplerDirtyMask |= TrackedPixelSamplerMask() | m_PixelForcedSamplerMask;
        m_LastSamplerConfigHash = configHash;
    }
    m_AFGraphicsConfig = gfx;
    m_AFEnabled = ce::sampler_override::IsAnisotropicOverrideEnabled(m_AFGraphicsConfig);
    m_LastGraphicsConfigVersion = configVersion;
}

uint32_t CWrapD3D11DeviceContext::TrackedPixelSamplerMask() const {
    return m_PixelTrackedSamplerMask;
}

uint32_t CWrapD3D11DeviceContext::PixelAFCandidateSamplerMask() const {
    if (!m_CurrentPixelShaderAFMetadata || !m_CurrentPixelShaderAFMetadata->metadata.available) {
        return 0;
    }
    return m_CurrentPixelAFCandidateMask & TrackedPixelSamplerMask();
}

uint32_t CWrapD3D11DeviceContext::DirtyMaskForPixelShaderResourceViewSlot(UINT slot) const {
    if (slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return 0;
    }
    if (!m_CurrentPixelShaderAFMetadata || !m_CurrentPixelShaderAFMetadata->metadata.available) {
        return m_PixelForcedSamplerMask;
    }
    return m_CurrentPixelShaderAFMetadata->textureSamplerMasks[slot] & m_CurrentPixelAFCandidateMask &
           TrackedPixelSamplerMask();
}

void CWrapD3D11DeviceContext::TrackShaderResources(UINT stageIndex, UINT startSlot, UINT numViews,
                                                   ID3D11ShaderResourceView* const* views) {
    if (stageIndex != kWrapperStagePS || startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
        return;
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    uint32_t dirtyMask = 0;
    bool configRefreshed = false;
    for (UINT i = 0; i < actualViews; ++i) {
        const UINT slot = startSlot + i;
        ID3D11ShaderResourceView* view = views ? views[i] : nullptr;
        if (m_TrackedSRVs[stageIndex][slot] == view) {
            continue;
        }
        if (!configRefreshed) {
            RefreshForcedAFConfig();
            configRefreshed = true;
        }
        dirtyMask |= DirtyMaskForPixelShaderResourceViewSlot(slot);
        if (view)
            view->AddRef();
        if (m_TrackedSRVs[stageIndex][slot])
            m_TrackedSRVs[stageIndex][slot]->Release();
        m_TrackedSRVs[stageIndex][slot] = view;
        m_PixelSRVAFState[slot] = view ? 0u : 1u;
        if (view && m_AFEnabled && m_pAFRealDevice) {
            const auto decision = GetWrapperForcedAFViewMetadata(view);
            m_PixelSRVAFState[slot] = decision == ce::sampler_override::D3D11ForcedAFResourceDecision::Allow
                                          ? kWrapperAFViewEligible
                                          : kWrapperAFViewIneligible;
        }
    }
    if (dirtyMask != 0) {
        m_PixelSamplerDirtyMask |= dirtyMask;
    }
}

uint32_t CWrapD3D11DeviceContext::TrackSamplers(UINT stageIndex, UINT startSlot, UINT numSamplers,
                                                ID3D11SamplerState* const* samplers) {
    if (stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
        return 0;
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    uint32_t changedMask = 0;
    for (UINT i = 0; i < actualSamplers; ++i) {
        const UINT slot = startSlot + i;
        ID3D11SamplerState* sampler = samplers ? samplers[i] : nullptr;
        if (m_TrackedSamplers[stageIndex][slot] == sampler) {
            continue;
        }
        changedMask |= (1u << slot);
        if (sampler)
            sampler->AddRef();
        if (m_TrackedSamplers[stageIndex][slot])
            m_TrackedSamplers[stageIndex][slot]->Release();
        m_TrackedSamplers[stageIndex][slot] = sampler;
        if (stageIndex == kWrapperStagePS) {
            if (sampler) {
                m_PixelTrackedSamplerMask |= (1u << slot);
            } else {
                m_PixelTrackedSamplerMask &= ~(1u << slot);
            }
            if (m_ForcedSamplerVariants[slot]) {
                m_ForcedSamplerVariants[slot]->Release();
                m_ForcedSamplerVariants[slot] = nullptr;
            }
            m_ForcedSamplerVariantResolved[slot] = false;
        }
    }
    if (stageIndex == kWrapperStagePS && changedMask != 0) {
        const uint32_t relevantMask = PixelAFCandidateSamplerMask() | m_PixelForcedSamplerMask;
        const uint32_t dirtyMask = changedMask & relevantMask;
        m_PixelSamplerDirtyMask |= dirtyMask;
    }
    return changedMask;
}

void CWrapD3D11DeviceContext::TrackPixelShader(ID3D11PixelShader* shader) {
    if (m_CurrentPixelShader == shader) {
        return;
    }
    WrapperPixelShaderAFMetadataHandle* metadata = AcquireWrapperPixelShaderAFMetadata(shader);
    if (shader) {
        shader->AddRef();
    }
    if (m_CurrentPixelShader) {
        m_CurrentPixelShader->Release();
    }
    if (m_CurrentPixelShaderAFMetadata) {
        m_CurrentPixelShaderAFMetadata->Release();
    }
    m_CurrentPixelShader = shader;
    m_CurrentPixelShaderAFMetadata = metadata;
    m_CurrentPixelAFCandidateMask = metadata ? metadata->candidateSamplerMask : 0;
    m_PixelSamplerDirtyMask |= PixelAFCandidateSamplerMask() | m_PixelForcedSamplerMask;
}

void CWrapD3D11DeviceContext::RefreshPixelShader() {
    if (!m_pReal) {
        return;
    }
    ID3D11PixelShader* shader = nullptr;
    UINT classInstanceCount = 0;
    m_pReal->PSGetShader(&shader, nullptr, &classInstanceCount);
    TrackPixelShader(shader);
    if (shader) {
        shader->Release();
    }
}

void CWrapD3D11DeviceContext::RefreshShaderResources(UINT stageIndex, UINT startSlot, UINT numViews) {
    if (!m_pReal || stageIndex != kWrapperStagePS || startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ||
        numViews == 0)
        return;
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    m_pReal->PSGetShaderResources(startSlot, actualViews, views);
    TrackShaderResources(stageIndex, startSlot, actualViews, views);
    for (UINT i = 0; i < actualViews; ++i) {
        if (views[i])
            views[i]->Release();
    }
}

ID3D11SamplerState* CWrapD3D11DeviceContext::ResolveForcedAFSampler(UINT stageIndex, UINT slot,
                                                                    ID3D11Device* realDevice,
                                                                    ID3D11SamplerState* original,
                                                                    const GraphicsConfig& gfx) {
    if (!original || !realDevice || stageIndex >= 6)
        return original;
    if (!ce::sampler_override::IsAnisotropicOverrideEnabled(gfx))
        return original;

    if (stageIndex != kWrapperStagePS) {
        return original;
    }
    if (slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return original;
    }

    if (!m_CurrentPixelShader) {
        RefreshPixelShader();
    }
    if (!m_CurrentPixelShader) {
        int idx = g_WrapperAFSkipNoShader.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12)
            WrapperLog("Wrapper: AF skip (no active pixel shader slot=%u)", slot);
        return original;
    }

    if (!m_CurrentPixelShaderAFMetadata || !m_CurrentPixelShaderAFMetadata->metadata.available) {
        int idx = g_WrapperAFSkipNoShaderMetadata.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24)
            WrapperLog(
                "Wrapper: AF skip (no pixel-shader sample metadata slot=%u has=%d failed=%d)", slot,
                m_CurrentPixelShaderAFMetadata ? 1 : 0,
                m_CurrentPixelShaderAFMetadata && m_CurrentPixelShaderAFMetadata->metadata.disassembleFailed ? 1 : 0);
        return original;
    }

    if ((m_CurrentPixelAFCandidateMask & (1u << slot)) == 0) {
        return original;
    }

    const UINT textureCount = m_CurrentPixelShaderAFMetadata->samplerTextureCounts[slot];
    for (UINT textureIndex = 0; textureIndex < textureCount; ++textureIndex) {
        const UINT textureSlot = m_CurrentPixelShaderAFMetadata->samplerTextureSlots[slot][textureIndex];

        ID3D11ShaderResourceView* view = m_TrackedSRVs[stageIndex][textureSlot];
        if (!view) {
            RefreshShaderResources(stageIndex, textureSlot, 1);
            view = m_TrackedSRVs[stageIndex][textureSlot];
        }
        if (!view) {
            int idx = g_WrapperAFSkipNoSRV.fetch_add(1, std::memory_order_relaxed);
            if (idx < 24)
                WrapperLog("Wrapper: AF skip (shader samples missing SRV s%u->t%u sampledTextures=%u)", slot,
                           textureSlot, textureCount);
            return original;
        }

        if (m_PixelSRVAFState[textureSlot] == kWrapperAFViewUnknown) {
            const auto decision = GetWrapperForcedAFViewMetadata(view);
            m_PixelSRVAFState[textureSlot] = decision == ce::sampler_override::D3D11ForcedAFResourceDecision::Allow
                                                 ? kWrapperAFViewEligible
                                                 : kWrapperAFViewIneligible;
        }
        if (m_PixelSRVAFState[textureSlot] != kWrapperAFViewEligible) {
            return original;
        }
    }

    if (!m_ForcedSamplerVariantResolved[slot]) {
        m_ForcedSamplerVariants[slot] = AcquireWrapperReplacementSampler(realDevice, original, gfx);
        m_ForcedSamplerVariantResolved[slot] = true;
    }
    return m_ForcedSamplerVariants[slot] ? m_ForcedSamplerVariants[slot] : original;
}

void CWrapD3D11DeviceContext::RememberRealSampler(UINT stageIndex, UINT slot, ID3D11SamplerState* sampler) {
    if (stageIndex >= 6 || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return;
    }
    ID3D11SamplerState*& tracked = m_RealSamplers[stageIndex][slot];
    if (tracked == sampler) {
        return;
    }
    if (sampler) {
        sampler->AddRef();
    }
    if (tracked) {
        tracked->Release();
    }
    tracked = sampler;
    if (stageIndex == kWrapperStagePS) {
        const uint32_t bit = (1u << slot);
        if (sampler && sampler != m_TrackedSamplers[kWrapperStagePS][slot]) {
            m_PixelForcedSamplerMask |= bit;
        } else {
            m_PixelForcedSamplerMask &= ~bit;
        }
    }
}

void CWrapD3D11DeviceContext::SetRealSamplerRange(UINT stageIndex, UINT startSlot, UINT numSamplers,
                                                  ID3D11SamplerState* const* samplers) {
    if (!m_pReal || stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSamplers == 0) {
        return;
    }

    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    if (actualSamplers == 0) {
        return;
    }

    {
        DX11WrapperForwardingScope forwardingScope;
        switch (stageIndex) {
            case kWrapperStagePS:
                m_pReal->PSSetSamplers(startSlot, actualSamplers, samplers);
                break;
            case kWrapperStageVS:
                m_pReal->VSSetSamplers(startSlot, actualSamplers, samplers);
                break;
            case kWrapperStageGS:
                m_pReal->GSSetSamplers(startSlot, actualSamplers, samplers);
                break;
            case kWrapperStageHS:
                m_pReal->HSSetSamplers(startSlot, actualSamplers, samplers);
                break;
            case kWrapperStageDS:
                m_pReal->DSSetSamplers(startSlot, actualSamplers, samplers);
                break;
            case kWrapperStageCS:
                m_pReal->CSSetSamplers(startSlot, actualSamplers, samplers);
                break;
        }
    }
    for (UINT i = 0; i < actualSamplers; ++i) {
        RememberRealSampler(stageIndex, startSlot + i, samplers ? samplers[i] : nullptr);
    }
}

void CWrapD3D11DeviceContext::ReconcileSamplers(UINT stageIndex, UINT startSlot, UINT numSlots, uint32_t slotMask) {
    const bool restoresForcedSampler = stageIndex == kWrapperStagePS && (slotMask & m_PixelForcedSamplerMask) != 0;
    if (!m_pReal || stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSlots == 0 ||
        slotMask == 0 || (!m_AFEnabled && !restoresForcedSampler))
        return;
    if (!m_pAFRealDevice)
        return;

    const UINT maxSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSlots = (numSlots < maxSlots) ? numSlots : maxSlots;
    ID3D11SamplerState* desiredSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    bool changedSlots[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    for (UINT i = 0; i < actualSlots; ++i) {
        const UINT slot = startSlot + i;
        if ((slotMask & (1u << slot)) == 0) {
            continue;
        }
        ID3D11SamplerState* logicalSampler = m_TrackedSamplers[stageIndex][slot];
        ID3D11SamplerState* desiredSampler = logicalSampler ? ResolveForcedAFSampler(stageIndex, slot, m_pAFRealDevice,
                                                                                     logicalSampler, m_AFGraphicsConfig)
                                                            : nullptr;
        if (m_RealSamplers[stageIndex][slot] != desiredSampler) {
            desiredSamplers[i] = desiredSampler;
            changedSlots[i] = true;
        }
    }
    for (UINT i = 0; i < actualSlots;) {
        if (!changedSlots[i]) {
            ++i;
            continue;
        }
        const UINT runStart = i;
        while (i < actualSlots && changedSlots[i]) {
            ++i;
        }
        SetRealSamplerRange(stageIndex, startSlot + runStart, i - runStart, &desiredSamplers[runStart]);
    }
}

void CWrapD3D11DeviceContext::PreparePixelSamplersForDraw() {
    RefreshForcedAFConfig();
    if (!m_AFEnabled) {
        if (m_PixelForcedSamplerMask == 0) {
            m_PixelSamplerDirtyMask = 0;
            return;
        }
        m_PixelSamplerDirtyMask |= m_PixelForcedSamplerMask;
    }

    const uint32_t dirtyMask = m_PixelSamplerDirtyMask;
    if (dirtyMask == 0) {
        return;
    }
    m_PixelSamplerDirtyMask = 0;
    ReconcileSamplers(kWrapperStagePS, 0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, dirtyMask);
}

void CWrapD3D11DeviceContext::BindTrackedSamplers(UINT stageIndex, UINT startSlot, UINT numSamplers,
                                                  ID3D11SamplerState* const* samplers) {
    if (!m_pReal) {
        return;
    }

    auto forwardUntrackedSamplerRange = [&]() {
        DX11WrapperForwardingScope forwardingScope;
        switch (stageIndex) {
            case kWrapperStagePS:
                m_pReal->PSSetSamplers(startSlot, numSamplers, samplers);
                break;
            case kWrapperStageVS:
                m_pReal->VSSetSamplers(startSlot, numSamplers, samplers);
                break;
            case kWrapperStageGS:
                m_pReal->GSSetSamplers(startSlot, numSamplers, samplers);
                break;
            case kWrapperStageHS:
                m_pReal->HSSetSamplers(startSlot, numSamplers, samplers);
                break;
            case kWrapperStageDS:
                m_pReal->DSSetSamplers(startSlot, numSamplers, samplers);
                break;
            case kWrapperStageCS:
                m_pReal->CSSetSamplers(startSlot, numSamplers, samplers);
                break;
        }
    };

    if (stageIndex != kWrapperStagePS || numSamplers == 0 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        forwardUntrackedSamplerRange();
        return;
    }

    const UINT actualSamplers = (numSamplers < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot)
                                    ? numSamplers
                                    : D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const uint32_t rangeMask = WrapperSamplerRangeMask(startSlot, actualSamplers);
    const uint32_t changedMask = TrackSamplers(stageIndex, startSlot, actualSamplers, samplers);
    RefreshForcedAFConfig();

    if (!m_pAFRealDevice) {
        forwardUntrackedSamplerRange();
        for (UINT i = 0; i < actualSamplers; ++i) {
            RememberRealSampler(stageIndex, startSlot + i, samplers ? samplers[i] : nullptr);
        }
        return;
    }

    if (!m_AFEnabled) {
        ID3D11SamplerState* logicalSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
        bool changedSlots[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
        for (UINT i = 0; i < actualSamplers; ++i) {
            const UINT slot = startSlot + i;
            logicalSamplers[i] = m_TrackedSamplers[kWrapperStagePS][slot];
            changedSlots[i] = m_RealSamplers[kWrapperStagePS][slot] != logicalSamplers[i];
        }
        for (UINT i = 0; i < actualSamplers;) {
            if (!changedSlots[i]) {
                ++i;
                continue;
            }
            const UINT runStart = i;
            while (i < actualSamplers && changedSlots[i]) {
                ++i;
            }
            SetRealSamplerRange(kWrapperStagePS, startSlot + runStart, i - runStart, &logicalSamplers[runStart]);
        }
        m_PixelSamplerDirtyMask &= ~rangeMask;
        return;
    }

    const uint32_t resolveMask = (changedMask | m_PixelSamplerDirtyMask) & rangeMask;
    if (resolveMask != 0) {
        m_PixelSamplerDirtyMask &= ~resolveMask;
        ReconcileSamplers(kWrapperStagePS, startSlot, actualSamplers, resolveMask);
    }
    return;
}
