// The shared D3D11 forced-AF engine, slow path: the shader/view verdict for one
// slot and the replacement sampler it needs. Runs without any state held: it
// reads descriptors and private data and may create a sampler through the device.

#include "d3d11_af_engine.h"

#include "dx11_hook_internal.h"

#include "hook/overrides/sampler_override/creation_policy.h"
#include "common/logging/log_meter.h"
#include "d3d11_af_metadata.h"
#include "d3d11_af_replacements.h"

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

// The replacement with final descriptor `desc` for `logical`, acquired: the one registered for
// the game's sampler in this epoch and verdict, else a new one, registered for it. Null on failure.
ID3D11SamplerState* AcquireReplacement(Host& host, ID3D11SamplerState* logical, const Policy& policy, bool allowAF,
                                       const D3D11_SAMPLER_DESC& desc, UINT slot) {
    if (ID3D11SamplerState* registered = AcquireRegisteredReplacement(logical, policy.epoch, allowAF)) {
        return registered;
    }
    ID3D11Device* device = host.AcquireDevice();
    if (!device) {
        return nullptr;
    }
    ID3D11SamplerState* replacement = nullptr;
    const HRESULT hr = host.CreateReplacement(device, desc, &replacement);
    device->Release();
    if (FAILED(hr) || !replacement) {
        CountReplacementCreateFailed();
        static ce::log_meter::ChangeGate failures;
        if (const auto verdict = failures.Observe(ce::log_meter::FieldKey(hr))) {
            const ReplacementStats stats = SnapshotReplacementStats();
            HookLogImportant("DX11: Replacement sampler creation FAILED hr=0x%08X (path=%s slot=%u entries=%lld "
                             "created=%llu)%s",
                             hr, host.PathName(), slot, static_cast<long long>(stats.entries),
                             static_cast<unsigned long long>(stats.created),
                             ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
        return nullptr;
    }
    CountReplacementCreated();
    RegisterReplacement(logical, policy.epoch, allowAF, replacement);
    if (SkipLogDue(dx11_hook_g_DiagSamplerReplacementCreated, 48)) {
        HookLog("DX11: Created replacement sampler (path=%s slot=%u Filter=0x%X Aniso=%u Bias=%.2f MinLOD=%.2f "
                "MaxLOD=%.2f)",
                host.PathName(), slot, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, desc.MinLOD, desc.MaxLOD);
    }
    return replacement;
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
    const GraphicsConfig& gfx = *policy.gfx;
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - filled by GetDesc
    D3D11_SAMPLER_DESC desc = {};
    logical->GetDesc(&desc);
    const D3D11_SAMPLER_DESC logicalDesc = desc;
    bool rebased = false;
    const bool creationPolicyApplied =
        ce::sampler_override::ResolveD3D11SamplerBindDesc(logical, policy.hash, desc, &rebased);
    // Only an object the game did not create (no creation tag) can be CE's own replacement.
    if (!creationPolicyApplied && !rebased && IsReplacementInEpoch(logical, policy.epoch)) {
        facts->valid = true;
        facts->isReplacement = true;
        return Keep(logical);
    }
    if (rebased) {
        static ce::log_meter::ChangeGate policyRebaseLog;
        if (const auto verdict = policyRebaseLog.Observe(policy.epoch)) {
            HookLog("DX11 AF: changed sampler policy starts from the application's descriptor "
                    "(path=%s epoch=%llu bias=%.2f->%.2f)%s", host.PathName(),
                    static_cast<unsigned long long>(policy.epoch), logicalDesc.MipLODBias, desc.MipLODBias,
                    ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }

    ChoiceFacts resolved;
    resolved.valid = true;
    resolved.descAllowsAF = ce::sampler_override::ClassifyD3D11SamplerForForcedAF(desc, gfx) ==
                            ce::sampler_override::D3D11ForcedAFSamplerDecision::Allow;
    resolved.allowAF = SamplerAllowsForcedAF(desc, gfx) && ShaderAndViewsAllowAF(host, input);
    if (creationPolicyApplied)
        ce::sampler_override::ApplyD3D11BindTimeAF(desc, gfx, resolved.allowAF);
    else
        DX11Hook_ApplySamplerOverrides(desc, gfx, resolved.allowAF);
    // Removing a creation-time setting may leave the application descriptor
    // unchanged by the new policy, but different from the logical object.
    if (SameSamplerDesc11(desc, logicalDesc)) {
        *facts = resolved;
        return Keep(logical);
    }

    ID3D11SamplerState* replacement =
        AcquireReplacement(host, logical, policy, resolved.allowAF, desc, input.slot);
    if (!replacement) {
        return Keep(logical);  // facts stay invalid: the next pass tries again
    }
    *facts = resolved;
    return replacement;
}

}  // namespace ce::d3d11_af
