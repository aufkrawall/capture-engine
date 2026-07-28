        *hasShader = false;
    }
    if (!context || !metadata) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    auto it = g_D3D11ContextStates.find(context);
    if (it == g_D3D11ContextStates.end()) {
        return false;
    }
    if (hasShader) {
        *hasShader = it->second.pixelShader != nullptr;
    }
    if (!it->second.hasPixelShaderMetadata) {
        return false;
    }
    *metadata = it->second.pixelShaderMetadata;
    return true;
}

static void RefreshPixelShaderFromContext11(ID3D11DeviceContext* context) {
    if (!context) {
        return;
    }

    ID3D11PixelShader* shader = nullptr;
    UINT classInstanceCount = 0;
    context->PSGetShader(&shader, nullptr, &classInstanceCount);
    UpdateTrackedPixelShader11(context, shader);
    if (shader) {
        shader->Release();
    }
}

static void RefreshStageShaderResourcesFromContext11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                     UINT startSlot, UINT numViews) {
    if (!context || startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || numViews == 0) {
        return;
    }
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    GetStageShaderResources11(context, stage, startSlot, actualViews, views);
    UpdateStageShaderResources(context, stage, startSlot, actualViews, views);
    for (UINT i = 0; i < actualViews; ++i) {
        if (views[i]) {
            views[i]->Release();
        }
    }
}

static void RefreshStageSamplersFromContext11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                              UINT numSamplers) {
    if (!context || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSamplers == 0) {
        return;
    }
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    ID3D11SamplerState* logicalSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    GetStageSamplers11(context, stage, startSlot, actualSamplers, samplers);
    for (UINT i = 0; i < actualSamplers; ++i) {
        logicalSamplers[i] = samplers[i];
        if (samplers[i] && IsReplacementSampler11(samplers[i])) {
            ID3D11SamplerState* trackedOriginal = GetTrackedSampler11(context, stage, startSlot + i);
            if (trackedOriginal) {
                logicalSamplers[i] = trackedOriginal;
            }
        }
    }
    UpdateStageSamplers(context, stage, startSlot, actualSamplers, logicalSamplers);
    RememberRealSamplerRange11(context, stage, startSlot, actualSamplers, samplers);
    for (UINT i = 0; i < actualSamplers; ++i) {
        if (logicalSamplers[i] && logicalSamplers[i] != samplers[i]) {
            logicalSamplers[i]->Release();
        }
        if (samplers[i]) {
            samplers[i]->Release();
        }
    }
}

static ce::sampler_override::D3D11ForcedAFResourceDecision ClassifyViewForForcedAF11(
    ID3D11Device* device, ID3D11ShaderResourceView* view,
    ce::sampler_override::D3D11Texture2DForcedAFInfo* outInfo = nullptr) {
    (void)device;
    return GetWrapperForcedAFViewMetadata(view, outInfo);
}

static bool SamplerAllowsForcedAF(const D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    using ce::sampler_override::D3D11ForcedAFSamplerDecision;
    const D3D11ForcedAFSamplerDecision decision = ce::sampler_override::ClassifyD3D11SamplerForForcedAF(desc, gfx);
    switch (decision) {
        case D3D11ForcedAFSamplerDecision::Allow:
            return true;
        case D3D11ForcedAFSamplerDecision::OverrideDisabled:
            return false;
        case D3D11ForcedAFSamplerDecision::FixedLOD: {
            int idx = g_DiagSamplerSkipNoMips.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: AF skip sampler (fixed/no mips) Filter=0x%X MaxLOD=%.1f MinLOD=%.1f",
                                 desc.Filter, desc.MaxLOD, desc.MinLOD);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::BorderAddress: {
            int idx = g_DiagSamplerSkipBorder.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: AF skip sampler (border address) Filter=0x%X U=%d V=%d W=%d", desc.Filter,
                                 desc.AddressU, desc.AddressV, desc.AddressW);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::ReductionFilter: {
            int idx = g_DiagSamplerSkipReduction.fetch_add(1, std::memory_order_relaxed);
            if (idx < 6) {
                HookLogImportant("DX11: AF skip sampler (reduction filter) Filter=0x%X", desc.Filter);
            }
            return false;
        }
        case D3D11ForcedAFSamplerDecision::ComparisonFilter: {
            int idx = g_DiagSamplerSkipComparison.fetch_add(1, std::memory_order_relaxed);
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

static bool ShouldForceAnisotropyForStageSlot(ID3D11Device* device, ID3D11DeviceContext* context,
                                              D3D11ShaderStage stage, UINT slot, const D3D11_SAMPLER_DESC& desc,
                                              const GraphicsConfig& gfx) {
    if (!SamplerAllowsForcedAF(desc, gfx)) {
        return false;
    }
    if (stage != D3D11ShaderStage::Pixel) {
        int idx = g_DiagSamplerSkipStage.fetch_add(1, std::memory_order_relaxed);
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
        int idx = g_DiagSamplerSkipNoShader.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (no active pixel shader, slot=%u)", slot);
        }
        return false;
    }

    if (!hasMetadata || !metadata.available) {
        int idx = g_DiagSamplerSkipNoShaderMetadata.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            HookLogImportant("DX11: AF skip sampler (no pixel-shader sample metadata, slot=%u has=%d failed=%d)", slot,
                             hasMetadata ? 1 : 0, metadata.disassembleFailed ? 1 : 0);
        }
        return false;
    }

    if (!ce::sampler_override::D3D11ShaderSamplerUsesAnyTexture(metadata.usage, slot)) {
        int idx = g_DiagSamplerSkipShaderUnused.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: AF skip sampler (pixel shader does not sample with s%u)", slot);
        }
        return false;
    }
    if (!ce::sampler_override::D3D11ShaderSamplerUsesAFSafeSample(metadata.usage, slot)) {
        int idx = g_DiagSamplerSkipExplicitSample.fetch_add(1, std::memory_order_relaxed);
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
            int idx = g_DiagSamplerSkipNoSRV.fetch_add(1, std::memory_order_relaxed);
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
            std::atomic<int>* counter = &g_DiagSamplerSkipUnsafeResource;
            const char* reason = ce::sampler_override::D3D11ForcedAFResourceDecisionName(resourceDecision);
            if (resourceDecision == ce::sampler_override::D3D11ForcedAFResourceDecision::UnsupportedFormat) {
                counter = &g_DiagSamplerSkipFormat;
            } else if (resourceDecision == ce::sampler_override::D3D11ForcedAFResourceDecision::SingleVisibleMip) {
                counter = &g_DiagSamplerSkipSingleMip;
            } else if (resourceDecision == ce::sampler_override::D3D11ForcedAFResourceDecision::NonColorFormat) {
                counter = &g_DiagSamplerSkipNonColorResource;
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

    int idx = g_DiagSamplerAllowsAF.fetch_add(1, std::memory_order_relaxed);
    if (ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot)) {
        g_DiagSamplerAllowLodSample.fetch_add(1, std::memory_order_relaxed);
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

bool DX11Hook_ApplySamplerOverrides(D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx,
                                    bool allowAnisotropicOverride) {
    bool modified = false;

    if (desc.MaxLOD <= 0.0f || desc.MinLOD >= desc.MaxLOD ||
        ce::sampler_override::IsD3D11ComparisonFilter(desc.Filter) ||
        ce::sampler_override::IsD3D11ReductionFilter(desc.Filter) || desc.AddressU == D3D11_TEXTURE_ADDRESS_BORDER ||
        desc.AddressV == D3D11_TEXTURE_ADDRESS_BORDER || desc.AddressW == D3D11_TEXTURE_ADDRESS_BORDER) {
        return false;
    }
    if (gfx.samplerOverrideMode != "aggressive") {
        if (D3D11_DECODE_MIN_FILTER(desc.Filter) != D3D11_FILTER_TYPE_LINEAR ||
            D3D11_DECODE_MAG_FILTER(desc.Filter) != D3D11_FILTER_TYPE_LINEAR) {
            return false;
        }
    }

    const std::string& af = gfx.anisotropicFiltering;
    if (af != "default" && !af.empty()) {
        if (af == "off") {
            if (ce::sampler_override::IsD3D11AnisotropicFilter(desc.Filter)) {
                desc.Filter = ce::sampler_override::IsD3D11ComparisonFilter(desc.Filter)
                                  ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR
                                  : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                desc.MaxAnisotropy = 1;
                modified = true;
                HookLogImportant("DX11: AF override OFF Filter=0x%X->0x%X Aniso=%u->1", desc.Filter, desc.Filter,
                                 desc.MaxAnisotropy);
            }
        } else if (allowAnisotropicOverride) {
            const UINT maxAniso = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);
            const D3D11_FILTER newFilter = ce::sampler_override::GetForcedAnisotropicFilter(desc.Filter);
            if (desc.Filter != newFilter || desc.MaxAnisotropy != maxAniso) {
                const D3D11_FILTER origFilter = desc.Filter;
                const UINT origAniso = desc.MaxAnisotropy;
                desc.Filter = newFilter;
                desc.MaxAnisotropy = maxAniso;
                modified = true;
                int idx = g_DiagSamplerAFApplied.fetch_add(1, std::memory_order_relaxed);
                if (idx < 48) {
                    HookLogImportant("DX11: AF override ON Filter=0x%X->0x%X Aniso=%u->%u (#%d)", origFilter,
                                     desc.Filter, origAniso, desc.MaxAnisotropy, idx + 1);
                }
            }
        }
    }

    const std::string& mip = gfx.mipMapping;
    const bool isAniso = ce::sampler_override::IsD3D11AnisotropicFilter(desc.Filter);
    if (mip != "default" && !isAniso) {
        if (mip == "trilinear") {
            if (desc.Filter != D3D11_FILTER_MIN_MAG_MIP_LINEAR) {
                desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                modified = true;
                int idx = g_DiagSamplerMipOverride.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Mip override trilinear applied (#%d)", idx + 1);
                }
            }
        } else if (mip == "bilinear") {
            if (desc.Filter != D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT) {
                desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                modified = true;
                int idx = g_DiagSamplerMipOverride.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Mip override bilinear applied (#%d)", idx + 1);
                }
            }
        }
    }

    float userBiasVal = 0.0f;
    const bool userBiasActive = TryParseConfiguredMipBias(gfx, userBiasVal);
    const float originalBias = desc.MipLODBias;
    desc.MipLODBias = ApplyConfiguredMipBias(gfx, originalBias);
    if (desc.MipLODBias != originalBias) {
        modified = true;
        int idx = g_DiagSamplerMipBiasApplied.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            HookLogImportant("DX11: Mip bias override Bias=%.2f->%.2f (#%d)", originalBias, desc.MipLODBias, idx + 1);
        }
    }

    if (gfx.sgssaa && !gfx.disableAutoMipBias && !gfx.forceMipBiasClamp) {
        float sgBias = 0.0f;
        if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgBias)) {
            desc.MipLODBias += sgBias;
            modified = true;
            HookLogImportant("DX11: SGSSAA bias applied (%.2f, total=%.2f)", sgBias, desc.MipLODBias);
        }
    }

    if (userBiasActive && userBiasVal < 0.0f && !gfx.sgssaa && IsUnityProcess() && !gfx.forceMipBiasClamp) {
        if (desc.MipLODBias < -0.5f) {
            desc.MipLODBias = -0.5f;
            modified = true;
            HookLogImportant("DX11: Unity mip bias clamp -0.5 applied");
        }
    }

    const float finalizedBias = FinalizeMipBias(gfx, desc.MipLODBias);
    if (finalizedBias != desc.MipLODBias) {
        desc.MipLODBias = finalizedBias;
        modified = true;
        HookLogImportant("DX11: Finalized mip bias %.2f->%.2f", desc.MipLODBias, finalizedBias);
    }

    return modified;
}

static ID3D11SamplerState* GetOrCreateReplacementSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                           UINT slot, ID3D11SamplerState* original) {
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
    const HRESULT hr = oCreateSamplerState ? oCreateSamplerState(device, &desc, &replacement)
                                           : device->CreateSamplerState(&desc, &replacement);

    if (FAILED(hr) || !replacement) {
        device->Release();
        int idx = g_DiagSamplerReplacementCreated.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLogImportant("DX11: Replacement sampler creation FAILED hr=0x%08X (stage=%d slot=%u)", hr, (int)stage,
                             slot);
        }
        return original;
    }

    AddReplacementSampler11(device, desc, replacement);
    AddToReplacementSet11(replacement);
    device->Release();
    int idx = g_DiagSamplerReplacementCreated.fetch_add(1, std::memory_order_relaxed);
    if (idx < 48) {
        HookLog("DX11: Created replacement sampler (stage=%d slot=%u Filter=0x%X Aniso=%u Bias=%.2f) #%d", (int)stage,
                slot, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, idx + 1);
    }
    return replacement;
}

static int ReconcileStageSamplers11(SetSamplers11_t originalFn, ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                    UINT startSlot, UINT numSlots, uint32_t slotMask) {
    if (!originalFn || !context || !g_GraphicsOverridesActive.load(std::memory_order_acquire) || g_InOverlayRender) {
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
            int idx = g_DiagSamplerRebound.fetch_add(1, std::memory_order_relaxed);
            if (idx < 48) {
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
        g_DiagSamplerReconcileSlots.fetch_add(visitedSlots, std::memory_order_relaxed);
    }
    return rebound;
}

static void SetSamplersWithOverrides11(SetSamplers11_t originalFn, ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                       UINT startSlot, UINT numSamplers, ID3D11SamplerState* const* ppSamplers) {
    if (!originalFn) {
        return;
    }
    if (DX11Hook_IsWrapperContextForwarding()) {
        originalFn(context, startSlot, numSamplers, ppSamplers);
        return;
    }
    if (numSamplers == 0 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT ||
        !g_GraphicsOverridesActive.load(std::memory_order_acquire) || g_InOverlayRender) {
        originalFn(context, startSlot, numSamplers, ppSamplers);
        RememberRealSamplerRange11(context, stage, startSlot, numSamplers, ppSamplers);
        return;
    }

    const UINT maxSamplers = static_cast<UINT>(D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    const UINT actualNum = (numSamplers < (maxSamplers - startSlot)) ? numSamplers : (maxSamplers - startSlot);
    const uint32_t changedMask = UpdateStageSamplers(context, stage, startSlot, actualNum, ppSamplers);
    int idx = g_DiagSamplerBindDeferred.fetch_add(1, std::memory_order_relaxed);
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
        int skipIdx = g_DiagSamplerEffectiveBindSkips.fetch_add(1, std::memory_order_relaxed);
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

    const int totalRebound = g_DiagSamplerEffectiveBinds.fetch_add(rebound, std::memory_order_relaxed) + rebound;
    int bindIdx = g_DiagSamplerEffectiveBindCalls.fetch_add(1, std::memory_order_relaxed);
    if (bindIdx < 48) {
        HookLog(
            "DX11: AF sampler bind effective stage=PS start=%u num=%u dirtyMask=0x%04X changedMask=0x%04X "
            "resolved=%d rebound=%d totalRebound=%d",
            startSlot, numSamplers, dirtyMask, changedMask, resolved, rebound, totalRebound);
    }
}

static void InstallContextVTableHooks11(ID3D11DeviceContext* context, const char* source);

static HRESULT STDMETHODCALLTYPE DetourCreatePixelShader11(ID3D11Device* device, const void* shaderBytecode,
                                                           SIZE_T bytecodeLength, ID3D11ClassLinkage* classLinkage,
                                                           ID3D11PixelShader** pixelShader) {
    const HRESULT hr = oCreatePixelShader11(device, shaderBytecode, bytecodeLength, classLinkage, pixelShader);
    if (SUCCEEDED(hr) && pixelShader && *pixelShader) {
        RegisterWrapperPixelShaderAFMetadata(*pixelShader, shaderBytecode, bytecodeLength);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourCreateDeferredContext11(ID3D11Device* device, UINT contextFlags,
                                                               ID3D11DeviceContext** deferredContext) {
    if (!oCreateDeferredContext11) {
        return E_FAIL;
    }
    const HRESULT hr = oCreateDeferredContext11(device, contextFlags, deferredContext);
    if (SUCCEEDED(hr) && deferredContext && *deferredContext) {
        InstallContextVTableHooks11(*deferredContext, "CreateDeferredContext");
        int idx = g_DiagCreateDeferredContext11.fetch_add(1, std::memory_order_relaxed);
        if (idx < 16) {
            void** vtable = *reinterpret_cast<void***>(*deferredContext);
            HookLogImportant("DX11: CreateDeferredContext returned ctx=%p flags=0x%X vtable=%p (#%d)",
                             (void*)*deferredContext, contextFlags, (void*)vtable, idx + 1);
        }
    }
    return hr;
}

static void STDMETHODCALLTYPE DetourPSSetShader11(ID3D11DeviceContext* context, ID3D11PixelShader* pixelShader,
                                                  ID3D11ClassInstance* const* classInstances, UINT numClassInstances) {
    PSSetShader11_t original =
        ResolveContextOriginal11(context, 9, &D3D11ContextVTableOriginals::psSetShader, oPSSetShader11);
    if (!original) {
        return;
    }
    original(context, pixelShader, classInstances, numClassInstances);
    if (g_InOverlayRender || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateTrackedPixelShader11(context, pixelShader);
}

static void STDMETHODCALLTYPE DetourPSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 8, &D3D11ContextVTableOriginals::psSetShaderResources, oPSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Pixel, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourVSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 25, &D3D11ContextVTableOriginals::vsSetShaderResources, oVSSetShaderResources11);
    if (!original) {
        return;
    }
