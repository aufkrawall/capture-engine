// The shared D3D11 forced-AF engine, slow path: the shader/view verdict for one
// slot and the replacement sampler it needs. Runs without any state held: it
// reads descriptors and private data and may create a sampler through the device.

#include "d3d11_af_engine.h"

#include "dx11_hook_internal.h"

#include "hook/overrides/sampler_override/creation_policy.h"
#include "hook/overrides/sampler_override/translation_scope.h"
#include "common/logging/log_meter.h"
#include "d3d11_af_metadata.h"

namespace ce::d3d11_af {

using ce::sampler_override::D3D11ForcedAFResourceDecision;

namespace {

bool SkipLogDue(std::atomic<int>& counter, int cap) {
    return counter.fetch_add(1, std::memory_order_relaxed) < cap;
}

// Whether the slot's shader and views allow forced AF, given a sampler descriptor that does.
bool ShaderAndViewsAllowAF(const Host& host, const SlowSlot& input) {
    const char* path = host.PathName();
    if (!input.hasShader) {
        if (SkipLogDue(dx11_hook_g_DiagSamplerSkipNoShader, 12)) {
            HookLogImportant("DX11: AF skip sampler (no active pixel shader, path=%s slot=%u)", path, input.slot);
        }
        return false;
    }
    if (!input.hasMetadata) {
        if (SkipLogDue(dx11_hook_g_DiagSamplerSkipNoShaderMetadata, 24)) {
            HookLogImportant("DX11: AF skip sampler (no pixel-shader sample metadata, path=%s slot=%u failed=%d)",
                             path, input.slot, input.disassembleFailed ? 1 : 0);
        }
        return false;
    }
    const auto& plan = input.plan;
    if (plan.textures.Count() == 0) {
        if (SkipLogDue(dx11_hook_g_DiagSamplerSkipShaderUnused, 12)) {
            HookLogImportant("DX11: AF skip sampler (pixel shader does not sample with s%u, path=%s)", input.slot,
                             path);
        }
        return false;
    }
    if (!plan.SafeSample()) {
        if (SkipLogDue(dx11_hook_g_DiagSamplerSkipExplicitSample, 24)) {
            HookLogImportant(
                "DX11: AF skip sampler (pixel shader uses non-implicit sample opcode with s%u path=%s implicit=%d "
                "bias=%d lod=%d grad=%d comp=%d other=%d explicit=%d)",
                input.slot, path, plan.implicitSample ? 1 : 0, plan.biasSample ? 1 : 0, plan.lodSample ? 1 : 0,
                plan.gradientSample ? 1 : 0, plan.comparisonSample ? 1 : 0, plan.otherExplicitSample ? 1 : 0,
                plan.explicitSample ? 1 : 0);
        }
        return false;
    }
    const UINT textureCount = plan.textures.Count();
    ce::sampler_override::D3D11Texture2DForcedAFInfo first = {};
    bool haveFirst = false;
    auto textures = plan.textures;
    for (UINT textureSlot = textures.TakeFirst(); textureSlot != UINT_MAX; textureSlot = textures.TakeFirst()) {
        ID3D11ShaderResourceView* view = input.views[textureSlot];
        if (!view) {
            if (SkipLogDue(dx11_hook_g_DiagSamplerSkipNoSRV, 24)) {
                HookLogImportant(
                    "DX11: AF skip sampler (shader samples missing SRV s%u->t%u, path=%s sampledTextures=%u)",
                    input.slot, textureSlot, path, textureCount);
            }
            return false;
        }
        ce::sampler_override::D3D11Texture2DForcedAFInfo info = {};
        const auto decision = ClassifyView(view, &info);
        if (decision != D3D11ForcedAFResourceDecision::Allow) {
            std::atomic<int>* counter = &dx11_hook_g_DiagSamplerSkipUnsafeResource;
            if (decision == D3D11ForcedAFResourceDecision::UnsupportedFormat) {
                counter = &dx11_hook_g_DiagSamplerSkipFormat;
            } else if (decision == D3D11ForcedAFResourceDecision::SingleVisibleMip) {
                counter = &dx11_hook_g_DiagSamplerSkipSingleMip;
            } else if (decision == D3D11ForcedAFResourceDecision::NonColorFormat) {
                counter = &dx11_hook_g_DiagSamplerSkipNonColorResource;
            }
            if (SkipLogDue(*counter, 24)) {
                HookLogImportant(
                    "DX11: AF skip sampler (%s, path=%s srvFmt=%d texFmt=%d dim=%d size=%ux%u mips=%u viewMip=%u "
                    "mostMip=%u array=%u samples=%u bind=0x%X misc=0x%X sampler=s%u texture=t%u sampledTextures=%u)",
                    ce::sampler_override::D3D11ForcedAFResourceDecisionName(decision), path, info.format,
                    info.textureFormat, info.viewDimension, info.width, info.height, info.mipLevels, info.viewMipLevels,
                    info.mostDetailedMip, info.arraySize, info.sampleCount, info.bindFlags, info.miscFlags, input.slot,
                    textureSlot, textureCount);
            }
            return false;
        }
        if (!haveFirst) {
            first = info;
            haveFirst = true;
        }
    }
    if (plan.lodSample) {
        dx11_hook_g_DiagSamplerAllowLodSample.fetch_add(1, std::memory_order_relaxed);
    }
    if (SkipLogDue(dx11_hook_g_DiagSamplerAllowsAF, 48)) {
        HookLogImportant(
            "DX11: AF allow shader-paired sampler path=%s slot=s%u sampledTextures=%u first=t%u last=t%u "
            "sampleKinds(implicit=%d bias=%d lod=%d) srvFmt=%d texFmt=%d dim=%d size=%ux%u mips=%u viewMip=%u",
            path, input.slot, textureCount, plan.textures.First(), plan.textures.Last(), plan.implicitSample ? 1 : 0,
            plan.biasSample ? 1 : 0, plan.lodSample ? 1 : 0, first.format, first.textureFormat, first.viewDimension,
            first.width, first.height, first.mipLevels, first.viewMipLevels);
    }
    return true;
}

ID3D11SamplerState* Keep(ID3D11SamplerState* logical) {
    logical->AddRef();
    return logical;
}

}  // namespace

ID3D11SamplerState* ResolveSlow(Host& host, const SlowSlot& input, const Policy& policy, ChoiceFacts* facts) {
    *facts = {};
    ID3D11SamplerState* logical = input.logical;
    if (!logical) {
        return nullptr;
    }
    if (!policy.active || !policy.gfx) {
        return Keep(logical);
    }
    if (IsReplacementSampler11(logical)) {
        facts->valid = true;
        facts->isReplacement = true;
        return Keep(logical);
    }
    const GraphicsConfig& gfx = *policy.gfx;
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - filled by GetDesc
    D3D11_SAMPLER_DESC desc = {};
    logical->GetDesc(&desc);

    ChoiceFacts resolved;
    resolved.valid = true;
    resolved.descAllowsAF = ce::sampler_override::ClassifyD3D11SamplerForForcedAF(desc, gfx) ==
                            ce::sampler_override::D3D11ForcedAFSamplerDecision::Allow;
    resolved.allowAF = SamplerAllowsForcedAF(desc, gfx) && ShaderAndViewsAllowAF(host, input);
    // A sampler created through CE carries its creation policy: only the AF decision is left.
    // One created before CE attached (or under another config) gets the whole policy here.
    const bool creationPolicyApplied = ce::sampler_override::HasD3D11CreationSamplerOverride(gfx) &&
                                       ce::sampler_override::HasCurrentD3D11SamplerCreationPolicy(logical, gfx);
    const bool modified = creationPolicyApplied
                              ? ce::sampler_override::ApplyD3D11BindTimeAF(desc, gfx, resolved.allowAF)
                              : DX11Hook_ApplySamplerOverrides(desc, gfx, resolved.allowAF);
    if (!modified) {
        *facts = resolved;
        return Keep(logical);
    }

    ID3D11Device* device = host.AcquireDevice();
    if (!device) {
        return Keep(logical);  // facts stay invalid: the next pass tries again
    }
    if (ID3D11SamplerState* cached = FindReplacementSampler11(device, desc)) {
        device->Release();
        *facts = resolved;
        return Keep(cached);
    }
    ID3D11SamplerState* replacement = nullptr;
    const HRESULT hr = host.CreateReplacement(device, desc, &replacement);
    if (FAILED(hr) || !replacement) {
        device->Release();
        if (SkipLogDue(dx11_hook_g_DiagSamplerReplacementCreated, 12)) {
            HookLogImportant("DX11: Replacement sampler creation FAILED hr=0x%08X (path=%s slot=%u)", hr,
                             host.PathName(), input.slot);
        }
        return Keep(logical);
    }
    // The replacement set owns the creation reference (ClearReplacementSamplerCache11Unlocked).
    AddReplacementSampler11(device, desc, replacement);
    AddToReplacementSet11(replacement);
    device->Release();
    *facts = resolved;
    if (SkipLogDue(dx11_hook_g_DiagSamplerReplacementCreated, 48)) {
        HookLog("DX11: Created replacement sampler (path=%s slot=%u Filter=0x%X Aniso=%u Bias=%.2f)",
                host.PathName(), input.slot, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias);
    }
    return Keep(replacement);
}


HRESULT CreateSamplerWithCreationPolicy(const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** sampler,
                                        CreateSamplerFn create, void* createContext, const char* path) {
    ce::sampler_override::D3D11SamplerCreationScope translationScope;
    if (!desc || HookIsShuttingDown() || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        return create(createContext, desc, sampler);
    }
    const GraphicsConfig& gfx = GetActiveGraphicsConfig();
    D3D11_SAMPLER_DESC applied = *desc;
    // Forced AF needs the shader and the texture, so it waits for the bind; creation
    // handles AF off, mip mapping and mip bias.
    const bool modified = DX11Hook_ApplySamplerOverrides(applied, gfx, false);
    static std::atomic<int> s_createLogs{0};
    const int idx = s_createLogs.fetch_add(1, std::memory_order_relaxed);
    if (idx < 48 && (modified || ce::sampler_override::IsAnisotropicOverrideEnabled(gfx))) {
        HookLogImportant(
            "DX11: CreateSamplerState %s path=%s Filter=0x%X->0x%X Aniso=%u->%u Bias=%.2f Addr=%d/%d/%d Comp=%d "
            "MinLOD=%.1f MaxLOD=%.1f (#%d)",
            modified ? "override" : "deferred runtime AF", path ? path : "?", desc->Filter, applied.Filter,
            desc->MaxAnisotropy, applied.MaxAnisotropy, applied.MipLODBias, desc->AddressU, desc->AddressV,
            desc->AddressW, desc->ComparisonFunc, desc->MinLOD, desc->MaxLOD, idx + 1);
    }
    const HRESULT hr = create(createContext, modified ? &applied : desc, sampler);
    if (FAILED(hr) && modified) {
        static ce::log_meter::ChangeGate failures;
        if (const auto verdict = failures.Observe(ce::log_meter::FieldKey(hr))) {
            HookLogImportant(
                "DX11: CreateSamplerState with CE's policy failed hr=0x%08X path=%s Filter=0x%X Bias=%.2f%s", hr,
                path ? path : "?", applied.Filter, applied.MipLODBias,
                ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    if (SUCCEEDED(hr) && sampler && *sampler) {
        const HRESULT tagHr = ce::sampler_override::RecordD3D11SamplerCreationPolicy(*sampler, gfx);
        if (FAILED(tagHr)) {
            static ce::log_meter::ChangeGate tagFailures;
            if (const auto verdict = tagFailures.Observe(ce::log_meter::FieldKey(tagHr))) {
                HookLog("DX11 AF: sampler creation policy tag failed hr=0x%08X; using legacy bind policy%s", tagHr,
                        ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
            }
        }
    }
    return hr;
}

}  // namespace ce::d3d11_af
