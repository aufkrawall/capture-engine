#include "dx9_sampler_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "../common/sampler_override_utils.h"
#include "hook_common.h"
#include "lod_helper.h"

namespace ce::dx9_sampler_state {
namespace {

constexpr size_t kSamplerCount = 21;
constexpr size_t kStateCount = 9;
constexpr std::array<D3DSAMPLERSTATETYPE, kStateCount> kTrackedTypes = {
    D3DSAMP_ADDRESSU,       D3DSAMP_ADDRESSV,  D3DSAMP_ADDRESSW,
    D3DSAMP_MAGFILTER,      D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER,
    D3DSAMP_MIPMAPLODBIAS,  D3DSAMP_MAXMIPLEVEL,
    D3DSAMP_MAXANISOTROPY,
};

struct SamplerState {
    std::array<DWORD, kStateCount> logical = {
        D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_POINT, D3DTEXF_POINT,
        D3DTEXF_NONE,     0,                  0,                  1,
    };
    std::array<DWORD, kStateCount> physical = logical;
    UINT textureMipLevels = 0;
    bool textureBound = false;
    bool textureUsesAddressW = false;
    bool textureSupportsAnisotropy = false;
    bool initialized = false;
    bool bootstrapAttempted = false;
};

struct DeviceState {
    IDirect3DDevice9* device = nullptr;
    std::mutex mutex;
    std::array<SamplerState, kSamplerCount> samplers;
    UINT maxAnisotropy = 1;
    DWORD textureFilterCaps = 0;
    DWORD cubeTextureFilterCaps = 0;
    DWORD volumeTextureFilterCaps = 0;
    std::atomic<uint64_t> configHash{0};
    std::atomic<uint32_t> configVersion{0xFFFFFFFFu};
    std::atomic<bool> overrideActive{false};
};

std::mutex g_registryMutex;
std::vector<std::unique_ptr<DeviceState>> g_devices;
std::atomic<uint64_t> g_reconciliations{0};
std::atomic<uint64_t> g_driverWrites{0};
std::atomic<uint64_t> g_bootstrapQueries{0};
std::atomic<uint64_t> g_configChanges{0};
std::atomic<int> g_transitionLogCount{0};
std::atomic<int> g_failureLogCount{0};
std::atomic<int> g_bootstrapFailureLogCount{0};

thread_local IDirect3DDevice9* t_cachedDevice = nullptr;
thread_local DeviceState* t_cachedState = nullptr;

int NormalizeSampler(DWORD sampler) {
    if (sampler < 16) {
        return static_cast<int>(sampler);
    }
    if (sampler >= D3DDMAPSAMPLER && sampler <= D3DVERTEXTEXTURESAMPLER3) {
        return 16 + static_cast<int>(sampler - D3DDMAPSAMPLER);
    }
    return -1;
}

int StateIndex(D3DSAMPLERSTATETYPE type) {
    for (size_t i = 0; i < kTrackedTypes.size(); ++i) {
        if (kTrackedTypes[i] == type) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

DWORD DenormalizeSampler(size_t index) {
    return index < 16 ? static_cast<DWORD>(index) : D3DDMAPSAMPLER + static_cast<DWORD>(index - 16);
}

bool HasSamplerOverride(const GraphicsConfig& gfx) {
    return (!gfx.anisotropicFiltering.empty() && gfx.anisotropicFiltering != "default") ||
           (!gfx.mipMapping.empty() && gfx.mipMapping != "default") || HasConfiguredMipBias(gfx) ||
           gfx.forceMipBiasClamp || (gfx.sgssaa && !gfx.disableAutoMipBias);
}

DeviceState* FindOrCreateDevice(IDirect3DDevice9* device) {
    if (t_cachedDevice == device && t_cachedState) {
        return t_cachedState;
    }

    std::lock_guard<std::mutex> lock(g_registryMutex);
    for (const auto& entry : g_devices) {
        if (entry->device == device) {
            t_cachedDevice = device;
            t_cachedState = entry.get();
            return entry.get();
        }
    }

    auto entry = std::make_unique<DeviceState>();
    entry->device = device;
    D3DCAPS9 caps = {};
    if (device && SUCCEEDED(device->GetDeviceCaps(&caps))) {
        entry->maxAnisotropy = std::max<UINT>(1u, static_cast<UINT>(caps.MaxAnisotropy));
        entry->textureFilterCaps = caps.TextureFilterCaps;
        entry->cubeTextureFilterCaps = caps.CubeTextureFilterCaps;
        entry->volumeTextureFilterCaps = caps.VolumeTextureFilterCaps;
    }
    DeviceState* result = entry.get();
    g_devices.push_back(std::move(entry));
    t_cachedDevice = device;
    t_cachedState = result;
    return result;
}

void ResetSampler(SamplerState& state, bool defaultsAreKnown) {
    state = SamplerState{};
    state.initialized = defaultsAreKnown;
    state.bootstrapAttempted = defaultsAreKnown;
}

bool FilterCapsAllowAnisotropy(DWORD caps) {
    return (caps & D3DPTFILTERCAPS_MINFANISOTROPIC) != 0 && (caps & D3DPTFILTERCAPS_MAGFANISOTROPIC) != 0;
}

UINT FullMipCount(UINT width, UINT height, UINT depth = 1) {
    UINT levels = 1;
    UINT extent = std::max(width, std::max(height, depth));
    while (extent > 1) {
        extent >>= 1;
        ++levels;
    }
    return levels;
}

void UpdateTextureMetadata(const DeviceState& deviceState, SamplerState& state, IDirect3DBaseTexture9* texture) {
    state.textureBound = texture != nullptr;
    state.textureMipLevels = texture ? texture->GetLevelCount() : 0;
    state.textureUsesAddressW = false;
    state.textureSupportsAnisotropy = false;
    if (!texture)
        return;

    switch (texture->GetType()) {
        case D3DRTYPE_TEXTURE: {
            state.textureSupportsAnisotropy = FilterCapsAllowAnisotropy(deviceState.textureFilterCaps);
            if (state.textureMipLevels <= 1) {
                D3DSURFACE_DESC desc = {};
                auto* texture2D = static_cast<IDirect3DTexture9*>(texture);
                if (SUCCEEDED(texture2D->GetLevelDesc(0, &desc)) && (desc.Usage & D3DUSAGE_AUTOGENMIPMAP))
                    state.textureMipLevels = FullMipCount(desc.Width, desc.Height);
            }
            break;
        }
        case D3DRTYPE_CUBETEXTURE: {
            state.textureSupportsAnisotropy = FilterCapsAllowAnisotropy(deviceState.cubeTextureFilterCaps);
            if (state.textureMipLevels <= 1) {
                D3DSURFACE_DESC desc = {};
                auto* cubeTexture = static_cast<IDirect3DCubeTexture9*>(texture);
                if (SUCCEEDED(cubeTexture->GetLevelDesc(0, &desc)) && (desc.Usage & D3DUSAGE_AUTOGENMIPMAP))
                    state.textureMipLevels = FullMipCount(desc.Width, desc.Height);
            }
            break;
        }
        case D3DRTYPE_VOLUMETEXTURE: {
            state.textureUsesAddressW = true;
            state.textureSupportsAnisotropy = FilterCapsAllowAnisotropy(deviceState.volumeTextureFilterCaps);
            if (state.textureMipLevels <= 1) {
                D3DVOLUME_DESC desc = {};
                auto* volumeTexture = static_cast<IDirect3DVolumeTexture9*>(texture);
                if (SUCCEEDED(volumeTexture->GetLevelDesc(0, &desc)) && (desc.Usage & D3DUSAGE_AUTOGENMIPMAP))
                    state.textureMipLevels = FullMipCount(desc.Width, desc.Height, desc.Depth);
            }
            break;
        }
        default:
            break;
    }
}

bool BootstrapSampler(DeviceState& deviceState, DWORD sampler, SamplerState& state, GetSamplerStateFn getState) {
    if (state.initialized || state.bootstrapAttempted || !getState) {
        return state.initialized;
    }
    state.bootstrapAttempted = true;

    bool stateQueriesSucceeded = true;
    for (size_t i = 0; i < kTrackedTypes.size(); ++i) {
        DWORD value = state.logical[i];
        if (SUCCEEDED(getState(deviceState.device, sampler, kTrackedTypes[i], &value))) {
            state.logical[i] = value;
            state.physical[i] = value;
        } else {
            stateQueriesSucceeded = false;
        }
    }

    IDirect3DBaseTexture9* texture = nullptr;
    if (SUCCEEDED(deviceState.device->GetTexture(sampler, &texture)) && texture) {
        UpdateTextureMetadata(deviceState, state, texture);
        texture->Release();
    }
    state.initialized = stateQueriesSucceeded;
    g_bootstrapQueries.fetch_add(1, std::memory_order_relaxed);
    if (!stateQueriesSucceeded) {
        const int logIndex = g_bootstrapFailureLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 8) {
            HookLogImportant("DX9: Sampler bootstrap unavailable for s%u; leaving application state untouched (#%d)",
                             sampler, logIndex + 1);
        }
    }
    return state.initialized;
}

sampler_override::D3D9SamplerForcedAFInfo MakeAFInfo(const SamplerState& state, UINT deviceMaxAnisotropy,
                                                     const std::array<DWORD, kStateCount>& desired) {
    sampler_override::D3D9SamplerForcedAFInfo info = {};
    info.addressU = desired[0];
    info.addressV = desired[1];
    info.addressW = desired[2];
    info.magFilter = desired[3];
    info.minFilter = desired[4];
    info.mipFilter = desired[5];
    const UINT maxMipLevel = desired[7];
    info.textureMipLevels = state.textureMipLevels > maxMipLevel ? state.textureMipLevels - maxMipLevel : 0;
    info.deviceMaxAnisotropy = state.textureSupportsAnisotropy ? deviceMaxAnisotropy : 1;
    info.textureBound = state.textureBound;
    info.usesAddressW = state.textureUsesAddressW;
    return info;
}

std::array<DWORD, kStateCount> BuildDesiredState(const SamplerState& state, UINT deviceMaxAnisotropy,
                                                 const GraphicsConfig& gfx,
                                                 sampler_override::D3D9ForcedAFDecision* afDecision) {
    std::array<DWORD, kStateCount> desired = state.logical;
    const bool textureHasMips = state.textureBound && desired[7] < state.textureMipLevels &&
                                state.textureMipLevels - desired[7] > 1;
    const auto materialAddress = [](DWORD address) {
        return address == D3DTADDRESS_WRAP || address == D3DTADDRESS_MIRROR || address == D3DTADDRESS_CLAMP;
    };
    const bool safeAddress = gfx.samplerOverrideMode == "aggressive" ||
                             (materialAddress(desired[0]) && materialAddress(desired[1]) &&
                              (!state.textureUsesAddressW || materialAddress(desired[2])));

    if (textureHasMips && desired[5] != D3DTEXF_NONE && safeAddress) {
        if (gfx.mipMapping == "trilinear") {
            desired[3] = D3DTEXF_LINEAR;
            desired[4] = D3DTEXF_LINEAR;
            desired[5] = D3DTEXF_LINEAR;
        } else if (gfx.mipMapping == "bilinear") {
            desired[3] = D3DTEXF_LINEAR;
            desired[4] = D3DTEXF_LINEAR;
            desired[5] = D3DTEXF_POINT;
        } else if (gfx.mipMapping == "nearest") {
            desired[3] = D3DTEXF_POINT;
            desired[4] = D3DTEXF_POINT;
            desired[5] = D3DTEXF_POINT;
        }
    }

    const auto info = MakeAFInfo(state, deviceMaxAnisotropy, desired);
    const auto decision = sampler_override::ClassifyD3D9SamplerForForcedAF(info, gfx);
    if (afDecision) {
        *afDecision = decision;
    }
    if (decision == sampler_override::D3D9ForcedAFDecision::Allow) {
        desired[3] = D3DTEXF_ANISOTROPIC;
        desired[4] = D3DTEXF_ANISOTROPIC;
        desired[8] = sampler_override::ResolveD3D9ForcedAnisotropy(info, gfx);
    } else if (gfx.anisotropicFiltering == "off") {
        if (desired[3] == D3DTEXF_ANISOTROPIC) {
            desired[3] = D3DTEXF_LINEAR;
        }
        if (desired[4] == D3DTEXF_ANISOTROPIC) {
            desired[4] = D3DTEXF_LINEAR;
        }
        desired[8] = 1;
    }

    float bias = ApplyConfiguredMipBias(gfx, std::bit_cast<float>(state.logical[6]));
    if (gfx.sgssaa && !gfx.disableAutoMipBias && !gfx.forceMipBiasClamp) {
        float sgssaaBias = 0.0f;
        if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgssaaBias)) {
            bias += sgssaaBias;
        }
    }
    desired[6] = std::bit_cast<DWORD>(FinalizeMipBias(gfx, bias));
    return desired;
}

void LogTransition(DWORD sampler, const SamplerState& state, const std::array<DWORD, kStateCount>& desired,
                   sampler_override::D3D9ForcedAFDecision decision, const GraphicsConfig& gfx) {
    const int index = g_transitionLogCount.fetch_add(1, std::memory_order_relaxed);
    if (index >= 48) {
        return;
    }
    HookLogImportant(
        "DX9: Event-driven sampler reconcile s%u decision=%d texture=%d mips=%u min=%u->%u mag=%u->%u "
        "aniso=%u->%u policy=%s (#%d)",
        sampler, static_cast<int>(decision), state.textureBound ? 1 : 0, state.textureMipLevels, state.logical[4],
        desired[4], state.logical[3], desired[3], state.logical[8], desired[8], gfx.samplerOverrideMode.c_str(),
        index + 1);
}

bool WriteCompanionStates(IDirect3DDevice9* device, DWORD sampler, SamplerState& state,
                          const std::array<DWORD, kStateCount>& desired, int requestedIndex,
                          SetSamplerStateFn setState) {
    if (!setState) {
        return false;
    }
    bool succeeded = true;
    for (size_t i = 0; i < kTrackedTypes.size(); ++i) {
        if (static_cast<int>(i) == requestedIndex || state.physical[i] == desired[i]) {
            continue;
        }
        const HRESULT hr = setState(device, sampler, kTrackedTypes[i], desired[i]);
        if (SUCCEEDED(hr)) {
            state.physical[i] = desired[i];
            g_driverWrites.fetch_add(1, std::memory_order_relaxed);
        } else {
            succeeded = false;
            const int logIndex = g_failureLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logIndex < 12) {
                HookLogImportant("DX9: Sampler companion write failed s%u type=%u value=%u hr=0x%08X (#%d)",
                                 sampler, static_cast<unsigned>(kTrackedTypes[i]), desired[i], hr, logIndex + 1);
            }
        }
    }
    return succeeded;
}

bool ReconcileSampler(IDirect3DDevice9* device, DWORD sampler, SamplerState& state, const GraphicsConfig& gfx,
                      UINT deviceMaxAnisotropy, SetSamplerStateFn setState) {
    sampler_override::D3D9ForcedAFDecision decision = sampler_override::D3D9ForcedAFDecision::OverrideDisabled;
    const auto desired = BuildDesiredState(state, deviceMaxAnisotropy, gfx, &decision);
    if (desired != state.physical) {
        LogTransition(sampler, state, desired, decision, gfx);
        const bool succeeded = WriteCompanionStates(device, sampler, state, desired, -1, setState);
        g_reconciliations.fetch_add(1, std::memory_order_relaxed);
        return succeeded && state.physical == desired;
    }
    return true;
}

void RefreshConfigLocked(DeviceState& deviceState, SetSamplerStateFn setState, GetSamplerStateFn getState) {
    const uint32_t version = GetActiveGraphicsConfigVersion();
    if (deviceState.configVersion.load(std::memory_order_relaxed) == version &&
        deviceState.configHash.load(std::memory_order_relaxed) != 0) {
        return;
    }
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    const uint64_t hash = sampler_override::HashSamplerOverrideConfig(gfx);
    const bool active = HasSamplerOverride(gfx);
    if (deviceState.configHash.load(std::memory_order_relaxed) == hash &&
        deviceState.overrideActive.load(std::memory_order_relaxed) == active) {
        deviceState.configVersion.store(version, std::memory_order_release);
        return;
    }

    g_configChanges.fetch_add(1, std::memory_order_relaxed);
    bool complete = true;
    for (size_t i = 0; i < deviceState.samplers.size(); ++i) {
        SamplerState& sampler = deviceState.samplers[i];
        const DWORD samplerIndex = DenormalizeSampler(i);
        if (active && !BootstrapSampler(deviceState, samplerIndex, sampler, getState)) {
            continue;
        }
        bool reconciled = true;
        if (sampler.initialized) {
            reconciled = ReconcileSampler(deviceState.device, samplerIndex, sampler, gfx,
                                          deviceState.maxAnisotropy, setState);
            complete = reconciled && complete;
        }
        if (!active && reconciled) {
            ResetSampler(sampler, false);
        }
    }
    deviceState.overrideActive.store(active || !complete, std::memory_order_release);
    if (complete) {
        deviceState.configHash.store(hash, std::memory_order_relaxed);
        deviceState.configVersion.store(version, std::memory_order_release);
    } else {
        deviceState.configHash.store(0, std::memory_order_relaxed);
        deviceState.configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    }
}

}  // namespace

void RegisterDevice(IDirect3DDevice9* device, bool newDevice) {
    if (!device) {
        return;
    }
    DeviceState* deviceState = FindOrCreateDevice(device);
    if (!newDevice) {
        return;
    }

    std::lock_guard<std::mutex> lock(deviceState->mutex);
    D3DCAPS9 caps = {};
    if (SUCCEEDED(device->GetDeviceCaps(&caps))) {
        deviceState->maxAnisotropy = std::max<UINT>(1u, static_cast<UINT>(caps.MaxAnisotropy));
        deviceState->textureFilterCaps = caps.TextureFilterCaps;
        deviceState->cubeTextureFilterCaps = caps.CubeTextureFilterCaps;
        deviceState->volumeTextureFilterCaps = caps.VolumeTextureFilterCaps;
    }
    for (SamplerState& sampler : deviceState->samplers) {
        ResetSampler(sampler, true);
    }
    deviceState->configHash.store(0, std::memory_order_relaxed);
    deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    deviceState->overrideActive.store(false, std::memory_order_release);
}

HRESULT SetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value,
                        SetSamplerStateFn setState, GetSamplerStateFn getState) {
    const int samplerIndex = NormalizeSampler(sampler);
    const int stateIndex = StateIndex(type);
    if (!device || !setState || samplerIndex < 0 || stateIndex < 0) {
        return setState ? setState(device, sampler, type, value) : D3DERR_INVALIDCALL;
    }

    const GraphicsConfig& fastConfig = GetActiveGraphicsConfigCached();
    if (!HasSamplerOverride(fastConfig)) {
        return setState(device, sampler, type, value);
    }

    DeviceState* deviceState = FindOrCreateDevice(device);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    RefreshConfigLocked(*deviceState, setState, getState);
    SamplerState& state = deviceState->samplers[static_cast<size_t>(samplerIndex)];
    if (!BootstrapSampler(*deviceState, sampler, state, getState)) {
        return setState(device, sampler, type, value);
    }
    const DWORD previousLogical = state.logical[static_cast<size_t>(stateIndex)];
    state.logical[static_cast<size_t>(stateIndex)] = value;

    sampler_override::D3D9ForcedAFDecision decision = sampler_override::D3D9ForcedAFDecision::OverrideDisabled;
    const auto desired = BuildDesiredState(state, deviceState->maxAnisotropy, fastConfig, &decision);
    const HRESULT hr = setState(device, sampler, type, desired[static_cast<size_t>(stateIndex)]);
    if (SUCCEEDED(hr)) {
        state.physical[static_cast<size_t>(stateIndex)] = desired[static_cast<size_t>(stateIndex)];
        const bool companionsSucceeded =
            WriteCompanionStates(device, sampler, state, desired, stateIndex, setState);
        if (!companionsSucceeded) {
            deviceState->configHash.store(0, std::memory_order_relaxed);
            deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
        }
        g_driverWrites.fetch_add(1, std::memory_order_relaxed);
    } else {
        state.logical[static_cast<size_t>(stateIndex)] = previousLogical;
    }
    if (SUCCEEDED(hr) && desired != state.logical) {
        g_reconciliations.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}

HRESULT GetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value,
                        GetSamplerStateFn getState) {
    const int samplerIndex = NormalizeSampler(sampler);
    const int stateIndex = StateIndex(type);
    if (!device || !getState || !value || samplerIndex < 0 || stateIndex < 0) {
        return getState ? getState(device, sampler, type, value) : D3DERR_INVALIDCALL;
    }

    DeviceState* deviceState = FindOrCreateDevice(device);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    if (!deviceState->overrideActive.load(std::memory_order_acquire)) {
        return getState(device, sampler, type, value);
    }
    SamplerState& state = deviceState->samplers[static_cast<size_t>(samplerIndex)];
    if (!BootstrapSampler(*deviceState, sampler, state, getState)) {
        return getState(device, sampler, type, value);
    }
    *value = state.logical[static_cast<size_t>(stateIndex)];
    return D3D_OK;
}

HRESULT SetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture, SetTextureFn setTexture,
                   SetSamplerStateFn setState, GetSamplerStateFn getState) {
    const int samplerIndex = NormalizeSampler(stage);
    if (!device || !setTexture || samplerIndex < 0) {
        return setTexture ? setTexture(device, stage, texture) : D3DERR_INVALIDCALL;
    }

    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    if (!HasSamplerOverride(gfx)) {
        return setTexture(device, stage, texture);
    }

    DeviceState* deviceState = FindOrCreateDevice(device);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    RefreshConfigLocked(*deviceState, setState, getState);
    SamplerState& state = deviceState->samplers[static_cast<size_t>(samplerIndex)];
    if (!BootstrapSampler(*deviceState, stage, state, getState)) {
        return setTexture(device, stage, texture);
    }

    const HRESULT hr = setTexture(device, stage, texture);
    if (FAILED(hr)) {
        return hr;
    }
    UpdateTextureMetadata(*deviceState, state, texture);
    if (!ReconcileSampler(device, stage, state, gfx, deviceState->maxAnisotropy, setState)) {
        deviceState->configHash.store(0, std::memory_order_relaxed);
        deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    }
    return hr;
}

void RefreshConfiguration(IDirect3DDevice9* device, SetSamplerStateFn setState, GetSamplerStateFn getState) {
    if (!device || !setState) {
        return;
    }
    DeviceState* deviceState = FindOrCreateDevice(device);
    const uint32_t version = GetActiveGraphicsConfigVersion();
    if (deviceState->configVersion.load(std::memory_order_acquire) == version &&
        deviceState->configHash.load(std::memory_order_relaxed) != 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    RefreshConfigLocked(*deviceState, setState, getState);
}

void InvalidateDevice(IDirect3DDevice9* device) {
    if (!device) {
        return;
    }
    DeviceState* deviceState = FindOrCreateDevice(device);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    for (SamplerState& sampler : deviceState->samplers) {
        ResetSampler(sampler, false);
    }
    deviceState->configHash.store(0, std::memory_order_relaxed);
    deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
}

void ResetDevice(IDirect3DDevice9* device) {
    if (!device) {
        return;
    }
    DeviceState* deviceState = FindOrCreateDevice(device);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    for (SamplerState& sampler : deviceState->samplers) {
        ResetSampler(sampler, true);
    }
    deviceState->configHash.store(0, std::memory_order_relaxed);
    deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    deviceState->overrideActive.store(false, std::memory_order_release);
}

void LogSummary() {
    HookLog("DX9: Sampler override summary reconciliations=%llu driverWrites=%llu bootstrapQueries=%llu "
            "configChanges=%llu",
            static_cast<unsigned long long>(g_reconciliations.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_driverWrites.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_bootstrapQueries.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_configChanges.load(std::memory_order_relaxed)));
}

}  // namespace ce::dx9_sampler_state
