#include "dx11_hook_internal.h"

#include "d3d11_af_replacements.h"


bool SameSamplerDesc11(const D3D11_SAMPLER_DESC& a,  const D3D11_SAMPLER_DESC& b) {


    return a.Filter == b.Filter && a.AddressU == b.AddressU && a.AddressV == b.AddressV && a.AddressW == b.AddressW &&
           a.MipLODBias == b.MipLODBias && a.MaxAnisotropy == b.MaxAnisotropy && a.ComparisonFunc == b.ComparisonFunc &&
           // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison) - exact bitwise cache identity is intended
           std::memcmp(a.BorderColor, b.BorderColor, sizeof(a.BorderColor)) == 0 && a.MinLOD == b.MinLOD &&
           a.MaxLOD == b.MaxLOD;

}


// Every context reconsiders its choices, and the replacement registry lets go of the old
// epoch's replacements; contexts still holding one release it with their memo or binding.
void AdvanceSamplerPolicyEpoch11Unlocked() {


    dx11_hook_g_SamplerConfigHash11 = 0;
    dx11_hook_g_SamplerConfigHash11Fast.store(0, std::memory_order_release);
    const uint64_t epoch = dx11_hook_g_SamplerCacheEpoch11.fetch_add(1, std::memory_order_acq_rel) + 1;
    // Releases only samplers: no path of their destruction takes the sampler cache mutex.
    ce::d3d11_af::DropRegisteredReplacements(epoch);

}


void EnsureSamplerCacheFresh11(const GraphicsConfig& gfx) {


    EnsureSamplerCacheFreshForHash11(ce::sampler_override::HashSamplerOverrideConfig(gfx));

}


void EnsureSamplerCacheFreshForHash11(uint64_t configHash) {


    if (dx11_hook_g_SamplerConfigHash11Fast.load(std::memory_order_acquire) == configHash) {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    if (dx11_hook_g_SamplerConfigHash11 == configHash) {
        dx11_hook_g_SamplerConfigHash11Fast.store(configHash, std::memory_order_release);
        return;
    }
    AdvanceSamplerPolicyEpoch11Unlocked();
    dx11_hook_g_SamplerConfigHash11 = configHash;
    dx11_hook_g_SamplerConfigHash11Fast.store(configHash, std::memory_order_release);
    lock.unlock();
    HookLog("DX11 AF: raw-context sampler policy refreshed hash=0x%llX epoch=%llu shaderDependencies=compact "
            "resourceInvalidation=changed-slots-only pixelChoices=memoized-per-epoch",
            static_cast<unsigned long long>(configHash),
            static_cast<unsigned long long>(dx11_hook_g_SamplerCacheEpoch11.load(std::memory_order_acquire)));

}


size_t GetStageIndex(D3D11ShaderStage stage) {


    switch (stage) {
        case D3D11ShaderStage::Pixel:
            return 0;
        case D3D11ShaderStage::Vertex:
            return 1;
        case D3D11ShaderStage::Geometry:
            return 2;
        case D3D11ShaderStage::Hull:
            return 3;
        case D3D11ShaderStage::Domain:
            return 4;
        case D3D11ShaderStage::Compute:
        default:
            return 5;
    }

}


void GetStageShaderResources11(ID3D11DeviceContext* context,  D3D11ShaderStage stage,  UINT startSlot, 
                                      UINT numViews,  ID3D11ShaderResourceView** views) {


    if (!context || !views || numViews == 0) {
        return;
    }
    switch (stage) {
        case D3D11ShaderStage::Pixel:
            context->PSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Vertex:
            context->VSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Geometry:
            context->GSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Hull:
            context->HSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Domain:
            context->DSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Compute:
            context->CSGetShaderResources(startSlot, numViews, views);
            break;
    }

}


void GetStageSamplers11(ID3D11DeviceContext* context,  D3D11ShaderStage stage,  UINT startSlot,  UINT numSamplers, 
                               ID3D11SamplerState** samplers) {


    if (!context || !samplers || numSamplers == 0) {
        return;
    }
    switch (stage) {
        case D3D11ShaderStage::Pixel:
            context->PSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Vertex:
            context->VSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Geometry:
            context->GSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Hull:
            context->HSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Domain:
            context->DSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Compute:
            context->CSGetSamplers(startSlot, numSamplers, samplers);
            break;
    }

}


D3D11InternalIdentityProbeScope::~D3D11InternalIdentityProbeScope() {


        DX11Hook_EndInternalIdentityProbe();

}
