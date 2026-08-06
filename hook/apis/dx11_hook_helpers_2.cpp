#include "dx11_hook_internal.h"


ce::sampler_override::D3D11ForcedAFResourceDecision ClassifyViewForForcedAF11(
    ID3D11Device* device,  ID3D11ShaderResourceView* view, 
    ce::sampler_override::D3D11Texture2DForcedAFInfo* outInfo) {


    (void)device;
    return GetWrapperForcedAFViewMetadata(view, outInfo);

}

bool SamplerAllowsForcedAF(const D3D11_SAMPLER_DESC& desc,  const GraphicsConfig& gfx) {


    using ce::sampler_override::D3D11ForcedAFSamplerDecision;
    const D3D11ForcedAFSamplerDecision decision = ce::sampler_override::ClassifyD3D11SamplerForForcedAF(desc, gfx);
    switch (decision) {
        case D3D11ForcedAFSamplerDecision::Allow:
            return true;
        case D3D11ForcedAFSamplerDecision::OverrideDisabled:
            return false;
        case D3D11ForcedAFSamplerDecision::FixedLOD: {
            int idx = dx11_hook_g_DiagSamplerSkipNoMips.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: AF skip sampler (fixed/no mips) Filter=0x%X MaxLOD=%.1f MinLOD=%.1f",
                                 desc.Filter, desc.MaxLOD, desc.MinLOD);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::BorderAddress: {
            int idx = dx11_hook_g_DiagSamplerSkipBorder.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: AF skip sampler (border address) Filter=0x%X U=%d V=%d W=%d", desc.Filter,
                                 desc.AddressU, desc.AddressV, desc.AddressW);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::ReductionFilter: {
            int idx = dx11_hook_g_DiagSamplerSkipReduction.fetch_add(1, std::memory_order_relaxed);
            if (idx < 6) {
                HookLogImportant("DX11: AF skip sampler (reduction filter) Filter=0x%X", desc.Filter);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::ComparisonFilter: {
            int idx = dx11_hook_g_DiagSamplerSkipComparison.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: AF skip sampler (comparison filter) Filter=0x%X Func=%d Addr=%d/%d/%d",
                                 desc.Filter, desc.ComparisonFunc, desc.AddressU, desc.AddressV, desc.AddressW);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::PointMinMag:
            return false;
    }
    return false;

}

bool ShouldForceAnisotropyForStageSlot(ID3D11Device* device,  ID3D11DeviceContext* context, 
                                              D3D11ShaderStage stage,  UINT slot,  const D3D11_SAMPLER_DESC& desc, 
                                              const GraphicsConfig& gfx) {


    if (!SamplerAllowsForcedAF(desc, gfx)) {
        return false;
    }
    if (stage != D3D11ShaderStage::Pixel) {
        int idx = dx11_hook_g_DiagSamplerSkipStage.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (non-pixel stage=%s slot=%u)", GetStageName11(stage), slot);
        }
        return false;
    }
    if (slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return false;
    }

    WrapperPixelShaderAFMetadata metadata = {};
    bool hasShader = false;
    bool hasMetadata = GetTrackedPixelShaderMetadata11(context, &hasShader, &metadata);
    if (!hasShader) {
        RefreshPixelShaderFromContext11(context);
        hasMetadata = GetTrackedPixelShaderMetadata11(context, &hasShader, &metadata);
    }
    if (!hasShader) {
        int idx = dx11_hook_g_DiagSamplerSkipNoShader.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (no active pixel shader, slot=%u)", slot);
        }
        return false;
    }

    if (!hasMetadata || !metadata.available) {
        int idx = dx11_hook_g_DiagSamplerSkipNoShaderMetadata.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            HookLogImportant("DX11: AF skip sampler (no pixel-shader sample metadata, slot=%u has=%d failed=%d)", slot,
                             hasMetadata ? 1 : 0, metadata.disassembleFailed ? 1 : 0);
        }
        return false;
    }

    if (!ce::sampler_override::D3D11ShaderSamplerUsesAnyTexture(metadata.usage, slot)) {
        int idx = dx11_hook_g_DiagSamplerSkipShaderUnused.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (pixel shader does not sample with s%u)", slot);
        }
        return false;
    }
    if (!ce::sampler_override::D3D11ShaderSamplerUsesAFSafeSample(metadata.usage, slot)) {
        int idx = dx11_hook_g_DiagSamplerSkipExplicitSample.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            HookLogImportant(
                "DX11: AF skip sampler (pixel shader uses non-implicit sample opcode with s%u implicit=%d bias=%d "
                "lod=%d grad=%d comp=%d other=%d explicit=%d)",
                slot, metadata.usage.samplerUsesImplicitSample[slot] ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesBiasSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesGradientSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesComparisonSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesOtherExplicitSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesExplicitSample(metadata.usage, slot) ? 1 : 0);
        }
        return false;
    }

    UINT firstTextureSlot = UINT_MAX;
    UINT lastTextureSlot = UINT_MAX;
    const UINT textureCount = ce::sampler_override::CountD3D11ShaderSamplerTextureUses(
        metadata.usage, slot, &firstTextureSlot, &lastTextureSlot);
    D3D11_SHADER_RESOURCE_VIEW_DESC firstSrvDesc = {};
    ce::sampler_override::D3D11Texture2DForcedAFInfo firstResourceInfo = {};
    bool capturedFirstResource = false;

    for (UINT textureSlot = 0; textureSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++textureSlot) {
        if (!ce::sampler_override::D3D11ShaderSamplerUsesTexture(metadata.usage, slot, textureSlot)) {
            continue;
        }

        ID3D11ShaderResourceView* view = GetTrackedShaderResourceView11(context, stage, textureSlot);
        if (!view) {
            RefreshStageShaderResourcesFromContext11(context, stage, textureSlot, 1);
            view = GetTrackedShaderResourceView11(context, stage, textureSlot);
        }
        if (!view) {
            int idx = dx11_hook_g_DiagSamplerSkipNoSRV.fetch_add(1, std::memory_order_relaxed);
            if (idx < 24) {
                HookLogImportant("DX11: AF skip sampler (shader samples missing SRV s%u->t%u, sampledTextures=%u)",
                                 slot, textureSlot, textureCount);
            }
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        view->GetDesc(&srvDesc);
        ce::sampler_override::D3D11Texture2DForcedAFInfo resourceInfo = {};
        const auto resourceDecision = ClassifyViewForForcedAF11(device, view, &resourceInfo);
        if (resourceDecision != ce::sampler_override::D3D11ForcedAFResourceDecision::Allow) {
            std::atomic<int>* counter = &dx11_hook_g_DiagSamplerSkipUnsafeResource;
            const char* reason = ce::sampler_override::D3D11ForcedAFResourceDecisionName(resourceDecision);
            if (resourceDecision == ce::sampler_override::D3D11ForcedAFResourceDecision::UnsupportedFormat) {
                counter = &dx11_hook_g_DiagSamplerSkipFormat;
            } else if (resourceDecision == ce::sampler_override::D3D11ForcedAFResourceDecision::SingleVisibleMip) {
                counter = &dx11_hook_g_DiagSamplerSkipSingleMip;
            } else if (resourceDecision == ce::sampler_override::D3D11ForcedAFResourceDecision::NonColorFormat) {
                counter = &dx11_hook_g_DiagSamplerSkipNonColorResource;
            }
            int idx = counter->fetch_add(1, std::memory_order_relaxed);
            if (idx < 24) {
                HookLogImportant(
                    "DX11: AF skip sampler (%s, decision=%d srvFmt=%d sampleFmt=%d texFmt=%d dim=%d size=%ux%u mips=%u "
                    "viewMip=%u mostMip=%u array=%u samples=%u bind=0x%X misc=0x%X sampler=s%u texture=t%u "
                    "sampledTextures=%u)",
                    reason, (int)resourceDecision, srvDesc.Format, resourceInfo.format, resourceInfo.textureFormat,
                    resourceInfo.viewDimension, resourceInfo.width, resourceInfo.height, resourceInfo.mipLevels,
                    resourceInfo.viewMipLevels, resourceInfo.mostDetailedMip, resourceInfo.arraySize,
                    resourceInfo.sampleCount, resourceInfo.bindFlags, resourceInfo.miscFlags, slot, textureSlot,
                    textureCount);
            }
            view->Release();
            return false;
        }

        if (!capturedFirstResource) {
            firstSrvDesc = srvDesc;
            firstResourceInfo = resourceInfo;
            capturedFirstResource = true;
        }
        view->Release();
    }

    int idx = dx11_hook_g_DiagSamplerAllowsAF.fetch_add(1, std::memory_order_relaxed);
    if (ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot)) {
        dx11_hook_g_DiagSamplerAllowLodSample.fetch_add(1, std::memory_order_relaxed);
    }
    if (idx < 48) {
        HookLogImportant(
            "DX11: AF allow shader-paired sampler slot=s%u sampledTextures=%u first=t%u last=t%u "
            "Filter=0x%X Aniso=%u Addr=%d/%d/%d sampleKinds(implicit=%d bias=%d lod=%d) "
            "srvFmt=%d sampleFmt=%d texFmt=%d dim=%d "
            "size=%ux%u mips=%u viewMip=%u "
            "mostMip=%u array=%u samples=%u bind=0x%X misc=0x%X (#%d)",
            slot, textureCount, firstTextureSlot, lastTextureSlot, desc.Filter, desc.MaxAnisotropy, desc.AddressU,
            desc.AddressV, desc.AddressW, metadata.usage.samplerUsesImplicitSample[slot] ? 1 : 0,
            ce::sampler_override::D3D11ShaderSamplerUsesBiasSample(metadata.usage, slot) ? 1 : 0,
            ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot) ? 1 : 0, firstSrvDesc.Format,
            firstResourceInfo.format, firstResourceInfo.textureFormat, firstResourceInfo.viewDimension,
            firstResourceInfo.width, firstResourceInfo.height, firstResourceInfo.mipLevels,
            firstResourceInfo.viewMipLevels, firstResourceInfo.mostDetailedMip, firstResourceInfo.arraySize,
            firstResourceInfo.sampleCount, firstResourceInfo.bindFlags, firstResourceInfo.miscFlags, idx + 1);
    }
    return true;

}

ID3D11SamplerState* GetOrCreateReplacementSampler11(ID3D11DeviceContext* context,  D3D11ShaderStage stage, 
                                                           UINT slot,  ID3D11SamplerState* original) {


    if (!context || !original) {
        return original;
    }

    if (IsReplacementSampler11(original)) {
        return original;
    }

    const auto& gfx = GetActiveGraphicsConfigCached();
    EnsureSamplerCacheFresh11(gfx);

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) {
        return original;
    }

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D11_SAMPLER_DESC desc = {};
    original->GetDesc(&desc);

    const bool allowAnisotropicOverride = ShouldForceAnisotropyForStageSlot(device, context, stage, slot, desc, gfx);
    const bool modified = DX11Hook_ApplySamplerOverrides(desc, gfx, allowAnisotropicOverride);
    if (!modified) {
        device->Release();
        return original;
    }

    if (ID3D11SamplerState* cached = FindReplacementSampler11(device, desc)) {
        device->Release();
        return cached;
    }

    ID3D11SamplerState* replacement = nullptr;
    const HRESULT hr = dx11_hook_oCreateSamplerState ? dx11_hook_oCreateSamplerState(device, &desc, &replacement)
                                           : device->CreateSamplerState(&desc, &replacement);

    if (FAILED(hr) || !replacement) {
        device->Release();
        int idx = dx11_hook_g_DiagSamplerReplacementCreated.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: Replacement sampler creation FAILED hr=0x%08X (stage=%d slot=%u)", hr, (int)stage,
                             slot);
        }
        return original;
    }

    AddReplacementSampler11(device, desc, replacement);
    AddToReplacementSet11(replacement);
    device->Release();
    int idx = dx11_hook_g_DiagSamplerReplacementCreated.fetch_add(1, std::memory_order_relaxed);
    if (idx < 48) {
        HookLog("DX11: Created replacement sampler (stage=%d slot=%u Filter=0x%X Aniso=%u Bias=%.2f) #%d", (int)stage,
                slot, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, idx + 1);
    }
    return replacement;

}

int ReconcileStageSamplers11(SetSamplers11_t originalFn,  ID3D11DeviceContext* context,  D3D11ShaderStage stage, 
                                    UINT startSlot,  UINT numSlots,  uint32_t slotMask) {


    if (!originalFn || !context || !g_GraphicsOverridesActive.load(std::memory_order_acquire) || dx11_hook_g_InOverlayRender) {
        return 0;
    }
    if (startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSlots == 0 || slotMask == 0) {
        return 0;
    }

    const UINT maxSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSlots = (numSlots < maxSlots) ? numSlots : maxSlots;
    int rebound = 0;
    int visitedSlots = 0;
    for (UINT i = 0; i < actualSlots; ++i) {
        const UINT slot = startSlot + i;
        if ((slotMask & (1u << slot)) == 0) {
            continue;
        }
        ++visitedSlots;
        ID3D11SamplerState* logicalSampler = GetTrackedSampler11(context, stage, slot);
        if (!logicalSampler) {
            continue;
        }

        ID3D11SamplerState* desiredSampler = GetOrCreateReplacementSampler11(context, stage, slot, logicalSampler);
        ID3D11SamplerState* rememberedSampler = GetRememberedRealSampler11(context, stage, slot);
        if (rememberedSampler != desiredSampler) {
            originalFn(context, slot, 1, &desiredSampler);
            RememberRealSampler11(context, stage, slot, desiredSampler);
            ++rebound;
            int idx = dx11_hook_g_DiagSamplerRebound.fetch_add(1, std::memory_order_relaxed);
            if (idx < 48) {
                // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
                D3D11_SAMPLER_DESC desc = {};
                desiredSampler->GetDesc(&desc);
                HookLogImportant("DX11: AF reconciled sampler stage=%s slot=%u Filter=0x%X Aniso=%u Bias=%.2f (#%d)",
                                 GetStageName11(stage), slot, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias,
                                 idx + 1);
            }
        }
        if (rememberedSampler) {
            rememberedSampler->Release();
        }
        logicalSampler->Release();
    }
    if (visitedSlots != 0) {
        dx11_hook_g_DiagSamplerReconcileSlots.fetch_add(visitedSlots, std::memory_order_relaxed);
    }
    return rebound;

}

void SetSamplersWithOverrides11(SetSamplers11_t originalFn,  ID3D11DeviceContext* context,  D3D11ShaderStage stage, 
                                       UINT startSlot,  UINT numSamplers,  ID3D11SamplerState* const* ppSamplers) {


    if (!originalFn) {
        return;
    }
    if (DX11Hook_IsWrapperContextForwarding()) {
        originalFn(context, startSlot, numSamplers, ppSamplers);
        return;
    }
    if (numSamplers == 0 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT ||
        !g_GraphicsOverridesActive.load(std::memory_order_acquire) || dx11_hook_g_InOverlayRender) {
        originalFn(context, startSlot, numSamplers, ppSamplers);
        RememberRealSamplerRange11(context, stage, startSlot, numSamplers, ppSamplers);
        return;
    }

    const UINT maxSamplers = static_cast<UINT>(D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    const UINT actualNum = (numSamplers < (maxSamplers - startSlot)) ? numSamplers : (maxSamplers - startSlot);
    const uint32_t changedMask = UpdateStageSamplers(context, stage, startSlot, actualNum, ppSamplers);
    int idx = dx11_hook_g_DiagSamplerBindDeferred.fetch_add(1, std::memory_order_relaxed);
    if (idx < 24) {
        HookLog("DX11: AF sampler bind tracked stage=%s start=%u num=%u changedMask=0x%04X (#%d)",
                GetStageName11(stage), startSlot, numSamplers, changedMask, idx + 1);
    }

    if (stage != D3D11ShaderStage::Pixel) {
        originalFn(context, startSlot, numSamplers, ppSamplers);
        RememberRealSamplerRange11(context, stage, startSlot, numSamplers, ppSamplers);
        return;
    }

    const uint32_t rangeMask = SamplerRangeMask11(startSlot, actualNum);
    const uint32_t dirtyMask = PeekPixelSamplerDirtyMask11(context, rangeMask);
    if (dirtyMask == 0) {
        int skipIdx = dx11_hook_g_DiagSamplerEffectiveBindSkips.fetch_add(1, std::memory_order_relaxed);
        if (skipIdx < 24) {
            HookLog("DX11: AF sampler bind skipped stage=PS start=%u num=%u rangeMask=0x%04X (#%d)", startSlot,
                    numSamplers, rangeMask, skipIdx + 1);
        }
        return;
    }

    int rebound = 0;
    int resolved = 0;
    for (UINT i = 0; i < actualNum; ++i) {
        const UINT slot = startSlot + i;
        const uint32_t bit = (1u << slot);
        if ((dirtyMask & bit) == 0) {
            continue;
        }
        ++resolved;
        ID3D11SamplerState* logicalSampler = GetTrackedSampler11(context, stage, slot);
        ID3D11SamplerState* desiredSampler =
            logicalSampler ? GetOrCreateReplacementSampler11(context, stage, slot, logicalSampler) : nullptr;
        ID3D11SamplerState* rememberedSampler = GetRememberedRealSampler11(context, stage, slot);
        if (rememberedSampler != desiredSampler) {
            originalFn(context, slot, 1, &desiredSampler);
            RememberRealSampler11(context, stage, slot, desiredSampler);
            ++rebound;
        }
        if (rememberedSampler) {
            rememberedSampler->Release();
        }
        if (logicalSampler) {
            logicalSampler->Release();
        }
    }
    ClearPixelSamplerDirtyMask11(context, rangeMask);

    const int totalRebound = dx11_hook_g_DiagSamplerEffectiveBinds.fetch_add(rebound, std::memory_order_relaxed) + rebound;
    int bindIdx = dx11_hook_g_DiagSamplerEffectiveBindCalls.fetch_add(1, std::memory_order_relaxed);
    if (bindIdx < 48) {
        HookLog(
            "DX11: AF sampler bind effective stage=PS start=%u num=%u dirtyMask=0x%04X changedMask=0x%04X "
            "resolved=%d rebound=%d totalRebound=%d",
            startSlot, numSamplers, dirtyMask, changedMask, resolved, rebound, totalRebound);
    }

}
