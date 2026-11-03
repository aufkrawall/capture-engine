#include "dx9_sampler_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/graphics/mip_mapping_policy.h"
#include "hook/overrides/sampler_override_utils.h"
#include "dx9_sampler_interception_policy.h"
#include "dx9_sampler_state_internal.h"
#include "hook/runtime/hook_common.h"
#include "hook/ddraw/lod_helper.h"

namespace ce::dx9_sampler_state {

using detail::DeviceState;
using detail::FindExistingDevice;
using detail::FindOrCreateDevice;
using detail::ShadowLock;

namespace {

std::mutex g_registryMutex;
std::vector<std::unique_ptr<DeviceState>> g_devices;
std::atomic<uint64_t> g_reconciliations{0};
std::atomic<uint64_t> g_driverWrites{0};
std::atomic<uint64_t> g_companionWrites{0};
std::atomic<uint64_t> g_bootstrapQueries{0};
std::atomic<uint64_t> g_configChanges{0};
std::atomic<uint64_t> g_externalResyncs{0};
std::atomic<uint64_t> g_trackedStateBlockApplies{0};
std::atomic<int> g_transitionLogCount{0};
std::atomic<int> g_failureLogCount{0};
std::atomic<int> g_bootstrapFailureLogCount{0};

thread_local IDirect3DDevice9* t_cachedDevice = nullptr;
thread_local DeviceState* t_cachedState = nullptr;

bool HasSamplerOverride(const GraphicsConfig& gfx) {
    return ParseSamplerConfig(gfx).overrideConfigured;
}

}  // namespace

namespace detail {

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
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
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

DeviceState* FindExistingDevice(IDirect3DDevice9* device) {
    if (t_cachedDevice == device)
        return t_cachedState;

    std::lock_guard<std::mutex> lock(g_registryMutex);
    for (const auto& entry : g_devices) {
        if (entry->device == device) {
            t_cachedDevice = device;
            t_cachedState = entry.get();
            return entry.get();
        }
    }
    return nullptr;
}

std::atomic<uint32_t> g_activeOrRecordingDeviceCount{0};

void UpdateDeviceActiveOrRecordingCountLocked(DeviceState& deviceState) {
    const bool shouldCount =
        deviceState.overrideActive.load(std::memory_order_relaxed) ||
        deviceState.recording.load(std::memory_order_relaxed);
    if (shouldCount && !deviceState.countedAsActiveOrRecording) {
        deviceState.countedAsActiveOrRecording = true;
        g_activeOrRecordingDeviceCount.fetch_add(1, std::memory_order_release);
    } else if (!shouldCount && deviceState.countedAsActiveOrRecording) {
        deviceState.countedAsActiveOrRecording = false;
        g_activeOrRecordingDeviceCount.fetch_sub(1, std::memory_order_release);
    }
}

void ForgetBoundTextures() {
    std::vector<DeviceState*> devices;
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        for (const auto& entry : g_devices)
            devices.push_back(entry.get());
    }
    for (DeviceState* deviceState : devices) {
        ShadowLock lock(*deviceState, SamplerPublication::kAllSamplers);
        for (SamplerState& sampler : deviceState->samplers) {
            sampler.boundTexture = nullptr;
            sampler.reconciled = false;
        }
    }
}

void RepublishLocked(DeviceState& deviceState) {
    if (deviceState.pendingRedundantSets.load(std::memory_order_relaxed) != 0) {
        const uint32_t redundant = deviceState.pendingRedundantSets.exchange(0, std::memory_order_relaxed);
        deviceState.interceptedSets += redundant;
        deviceState.redundantSets += redundant;
        deviceState.publishedRedundantSets += redundant;
    }
    if (deviceState.pendingTextureBinds.load(std::memory_order_relaxed) != 0)
        deviceState.publishedTextureBinds += deviceState.pendingTextureBinds.exchange(0, std::memory_order_relaxed);

    // Recording, no override, or a configuration not (completely) applied: the
    // device answers nothing from the publication, whichever samplers changed.
    const bool publishable = !deviceState.recording.load(std::memory_order_relaxed) &&
                             deviceState.overrideActive.load(std::memory_order_relaxed) &&
                             deviceState.configHash.load(std::memory_order_relaxed) != 0 &&
                             deviceState.configVersion.load(std::memory_order_relaxed) != 0xFFFFFFFFu;
    if (!publishable)
        deviceState.publication.Withdraw(SamplerPublication::kAllSamplers);
    RepublishClean(deviceState.publication, deviceState.samplers, deviceState.withdrawn, publishable);
    deviceState.withdrawn = 0;
}

const ParsedSamplerConfig& GetActiveParsedSamplerConfigCached() {
    struct Cache {
        uint32_t version = 0xFFFFFFFFu;
        ParsedSamplerConfig config{};
    };
    static thread_local Cache s_cache;
    const uint32_t curVersion = GetActiveGraphicsConfigVersion();
    if (s_cache.version != curVersion) {
        const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
        s_cache.config = ParseSamplerConfig(gfx);
        s_cache.version = curVersion;
    }
    return s_cache.config;
}

}  // namespace detail

bool ShouldInterceptFast() {
    const ParsedSamplerConfig& parsed = detail::GetActiveParsedSamplerConfigCached();
    if (parsed.overrideConfigured) {
        return true;
    }
    return detail::g_activeOrRecordingDeviceCount.load(std::memory_order_acquire) > 0;
}

bool OverrideConfigured() {
    return detail::GetActiveParsedSamplerConfigCached().overrideConfigured;
}

namespace {

void ResetSampler(SamplerState& state, bool defaultsAreKnown) {
    state = SamplerState{};
    state.initialized = defaultsAreKnown;
    state.bootstrapAttempted = defaultsAreKnown;
    state.boundTexture = nullptr;
    state.reconciled = false;
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
        detail::UpdateTextureMetadata(deviceState, state, texture);
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

bool RefreshPhysicalSamplerState(DeviceState& deviceState, DWORD sampler, SamplerState& state,
                                 GetSamplerStateFn getState) {
    if (!state.initialized)
        return BootstrapSampler(deviceState, sampler, state, getState);

    bool succeeded = true;
    for (size_t i = 0; i < kTrackedTypes.size(); ++i) {
        DWORD value = state.physical[i];
        if (SUCCEEDED(getState(deviceState.device, sampler, kTrackedTypes[i], &value))) {
            state.physical[i] = value;
        } else {
            succeeded = false;
        }
    }
    IDirect3DBaseTexture9* texture = nullptr;
    if (SUCCEEDED(deviceState.device->GetTexture(sampler, &texture))) {
        detail::UpdateTextureMetadata(deviceState, state, texture);
        if (texture)
            texture->Release();
    } else {
        succeeded = false;
    }
    return succeeded;
}

void LogTransition(DWORD sampler, const SamplerState& state, const std::array<DWORD, kStateCount>& desired,
                   sampler_override::D3D9ForcedAFDecision decision, const ParsedSamplerConfig& parsed) {
    const int index = g_transitionLogCount.fetch_add(1, std::memory_order_relaxed);
    if (index >= 48) {
        return;
    }
    HookLogImportant(
        "DX9: Event-driven sampler reconcile s%u decision=%d texture=%d mips=%u min=%u->%u mag=%u->%u "
        "mip=%u->%u aniso=%u->%u aggressive=%d (#%d)",
        sampler, static_cast<int>(decision), state.textureBound ? 1 : 0, state.textureMipLevels, state.logical[4],
        desired[4], state.logical[3], desired[3], state.logical[5], desired[5], state.logical[8], desired[8],
        parsed.aggressiveMode ? 1 : 0, index + 1);
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
            g_companionWrites.fetch_add(1, std::memory_order_relaxed);
        } else {
            succeeded = false;
            const int logIndex = g_failureLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logIndex < 12) {
                HookLogImportant("DX9: Sampler companion write failed s%u type=%u value=%u hr=0x%08X (#%d)", sampler,
                                 static_cast<unsigned>(kTrackedTypes[i]), desired[i], hr, logIndex + 1);
            }
        }
    }
    return succeeded;
}

bool ReconcileSampler(IDirect3DDevice9* device, DWORD sampler, SamplerState& state, const ParsedSamplerConfig& parsed,
                      UINT deviceMaxAnisotropy, SetSamplerStateFn setState) {
    sampler_override::D3D9ForcedAFDecision decision = sampler_override::D3D9ForcedAFDecision::OverrideDisabled;
    const auto desired = BuildDesiredState(state, deviceMaxAnisotropy, parsed, &decision);
    if (desired != state.physical) {
        LogTransition(sampler, state, desired, decision, parsed);
        const bool succeeded = WriteCompanionStates(device, sampler, state, desired, -1, setState);
        g_reconciliations.fetch_add(1, std::memory_order_relaxed);
        state.reconciled = (succeeded && state.physical == desired);
        return state.reconciled;
    }
    state.reconciled = true;
    return true;
}

void RefreshConfigLocked(DeviceState& deviceState, SetSamplerStateFn setState, GetSamplerStateFn getState) {
    // Writes made now would be recorded into the block, not applied.
    if (deviceState.recording.load(std::memory_order_relaxed))
        return;
    const uint32_t version = GetActiveGraphicsConfigVersion();
    if (deviceState.configVersion.load(std::memory_order_relaxed) == version &&
        deviceState.configHash.load(std::memory_order_relaxed) != 0) {
        return;
    }
    const ParsedSamplerConfig& parsed = detail::GetActiveParsedSamplerConfigCached();
    const uint64_t hash = parsed.configHash;
    const bool active = parsed.overrideConfigured;
    if (deviceState.configHash.load(std::memory_order_relaxed) == hash &&
        deviceState.overrideActive.load(std::memory_order_relaxed) == active) {
        deviceState.configVersion.store(version, std::memory_order_release);
        return;
    }

    g_configChanges.fetch_add(1, std::memory_order_relaxed);
    detail::WithdrawLocked(deviceState, SamplerPublication::kAllSamplers);
    deviceState.parsedConfig = parsed;
    bool complete = true;
    for (size_t i = 0; i < deviceState.samplers.size(); ++i) {
        SamplerState& sampler = deviceState.samplers[i];
        sampler.reconciled = false;
        const DWORD samplerIndex = DenormalizeSampler(i);
        if (active && !BootstrapSampler(deviceState, samplerIndex, sampler, getState)) {
            continue;
        }
        bool reconciled = true;
        if (sampler.initialized) {
            reconciled =
                ReconcileSampler(deviceState.device, samplerIndex, sampler, parsed, deviceState.maxAnisotropy, setState);
            complete = reconciled && complete;
        }
        if (!active && reconciled) {
            ResetSampler(sampler, false);
        }
    }
    deviceState.overrideActive.store(active || !complete, std::memory_order_release);
    detail::UpdateDeviceActiveOrRecordingCountLocked(deviceState);
    if (complete) {
        deviceState.configHash.store(hash, std::memory_order_relaxed);
        deviceState.configVersion.store(version, std::memory_order_release);
    } else {
        deviceState.configHash.store(0, std::memory_order_relaxed);
        deviceState.configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    }
}

}  // namespace

void RegisterDevice(IDirect3DDevice9* device, bool newDevice, bool intercepted) {
    if (!device) {
        return;
    }
    DeviceState* deviceState = FindOrCreateDevice(device);
    if (!newDevice) {
        return;
    }

    ShadowLock lock(*deviceState, SamplerPublication::kAllSamplers);
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3DCAPS9 caps = {};
    if (SUCCEEDED(device->GetDeviceCaps(&caps))) {
        deviceState->maxAnisotropy = std::max<UINT>(1u, static_cast<UINT>(caps.MaxAnisotropy));
        deviceState->textureFilterCaps = caps.TextureFilterCaps;
        deviceState->cubeTextureFilterCaps = caps.CubeTextureFilterCaps;
        deviceState->volumeTextureFilterCaps = caps.VolumeTextureFilterCaps;
    }
    const bool tracking = dx9_sampler_interception::ShadowTracksFromDefaults(
        intercepted, HasSamplerOverride(GetActiveGraphicsConfigCached()));
    for (SamplerState& sampler : deviceState->samplers) {
        ResetSampler(sampler, tracking);
    }
    deviceState->stateBlocks.clear();
    deviceState->recording.store(false, std::memory_order_relaxed);
    deviceState->configHash.store(0, std::memory_order_relaxed);
    deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    deviceState->overrideActive.store(false, std::memory_order_release);
    deviceState->textureMetaCache = {};

    static std::atomic<int> s_registerLogCount{0};
    if (s_registerLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
        HookLogImportant(
            "DX9: Sampler device registered %p (maxAniso=%u texFilterCaps=0x%08X cubeFilterCaps=0x%08X "
            "volFilterCaps=0x%08X minAniso=%d magAniso=%d tracking=%d intercepted=%d)",
            device, deviceState->maxAnisotropy, deviceState->textureFilterCaps,
            deviceState->cubeTextureFilterCaps, deviceState->volumeTextureFilterCaps,
            detail::FilterCapsAllowMinAnisotropy(deviceState->textureFilterCaps) ? 1 : 0,
            detail::FilterCapsAllowMagAnisotropy(deviceState->textureFilterCaps) ? 1 : 0, tracking ? 1 : 0,
            intercepted ? 1 : 0);
    }
}

HRESULT SetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value,
                        SetSamplerStateFn setState, GetSamplerStateFn getState) {
    const int samplerIndex = NormalizeSampler(sampler);
    const int stateIndex = StateIndex(type);
    if (!device || !setState || samplerIndex < 0 || stateIndex < 0) {
        return setState ? setState(device, sampler, type, value) : D3DERR_INVALIDCALL;
    }

    // The per-draw common case, answered without the lock
    // (dx9_sampler_publication.h): the sampler is reconciled and already holds
    // this value, under the configuration that is current.
    if (DeviceState* known = FindExistingDevice(device);
        known && known->publication.IsRedundantSet(static_cast<size_t>(samplerIndex), static_cast<size_t>(stateIndex),
                                                   value) &&
        known->configVersion.load(std::memory_order_relaxed) == GetActiveGraphicsConfigVersion()) {
        detail::CountRelaxed(known->pendingRedundantSets);
        return D3D_OK;
    }

    const ParsedSamplerConfig& parsed = detail::GetActiveParsedSamplerConfigCached();
    const bool overrideConfigured = parsed.overrideConfigured;
    DeviceState* deviceState = overrideConfigured ? FindOrCreateDevice(device) : FindExistingDevice(device);
    if (!deviceState || (!overrideConfigured && !deviceState->overrideActive.load(std::memory_order_acquire) &&
                         !deviceState->recording.load(std::memory_order_acquire))) {
        return setState(device, sampler, type, value);
    }

    ShadowLock lock(*deviceState, SamplerPublication::Bit(static_cast<size_t>(samplerIndex)));
    if (deviceState->recording.load(std::memory_order_relaxed)) {
        // Recorded, not applied: the block gets the application's own value and
        // the shadow stays what the device really holds. The Apply forces it.
        const HRESULT recordHr = setState(device, sampler, type, value);
        if (SUCCEEDED(recordHr)) {
            RecordSamplerState(deviceState->recordingSnapshot, static_cast<size_t>(samplerIndex),
                               static_cast<size_t>(stateIndex), value);
        }
        return recordHr;
    }
    RefreshConfigLocked(*deviceState, setState, getState);
    if (!overrideConfigured && !deviceState->overrideActive.load(std::memory_order_acquire))
        return setState(device, sampler, type, value);
    SamplerState& state = deviceState->samplers[static_cast<size_t>(samplerIndex)];
    ++deviceState->interceptedSets;
    if (IsRedundantLogicalSet(state, static_cast<size_t>(stateIndex), value)) {
        ++deviceState->redundantSets;
        return D3D_OK;
    }
    if (!BootstrapSampler(*deviceState, sampler, state, getState)) {
        return setState(device, sampler, type, value);
    }
    const DWORD previousLogical = state.logical[static_cast<size_t>(stateIndex)];
    state.logical[static_cast<size_t>(stateIndex)] = value;

    sampler_override::D3D9ForcedAFDecision decision = sampler_override::D3D9ForcedAFDecision::OverrideDisabled;
    const auto desired = BuildDesiredState(state, deviceState->maxAnisotropy, parsed, &decision);
    if (previousLogical == value && state.physical == desired) {
        state.reconciled = true;
        return D3D_OK;
    }
    const HRESULT hr = setState(device, sampler, type, desired[static_cast<size_t>(stateIndex)]);
    if (SUCCEEDED(hr)) {
        state.physical[static_cast<size_t>(stateIndex)] = desired[static_cast<size_t>(stateIndex)];
        const bool companionsSucceeded = WriteCompanionStates(device, sampler, state, desired, stateIndex, setState);
        state.reconciled = companionsSucceeded && state.physical == desired;
        if (!companionsSucceeded) {
            deviceState->configHash.store(0, std::memory_order_relaxed);
            deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
        }
        g_driverWrites.fetch_add(1, std::memory_order_relaxed);
        if (desired != state.logical) {
            g_reconciliations.fetch_add(1, std::memory_order_relaxed);
            LogTransition(sampler, state, desired, decision, parsed);
        }
    } else {
        state.logical[static_cast<size_t>(stateIndex)] = previousLogical;
    }
    return hr;
}

HRESULT GetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value,
                        GetSamplerStateFn getState, SetSamplerStateFn setState) {
    const int samplerIndex = NormalizeSampler(sampler);
    const int stateIndex = StateIndex(type);
    if (!device || !getState || !value || samplerIndex < 0 || stateIndex < 0) {
        return getState ? getState(device, sampler, type, value) : D3DERR_INVALIDCALL;
    }

    const ParsedSamplerConfig& parsed = detail::GetActiveParsedSamplerConfigCached();
    const bool overrideConfigured = parsed.overrideConfigured;
    DeviceState* deviceState = overrideConfigured ? FindOrCreateDevice(device) : FindExistingDevice(device);
    if (!deviceState || (!overrideConfigured && !deviceState->overrideActive.load(std::memory_order_acquire)))
        return getState(device, sampler, type, value);
    ShadowLock lock(*deviceState, SamplerPublication::Bit(static_cast<size_t>(samplerIndex)));
    RefreshConfigLocked(*deviceState, setState, getState);
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

    // A re-bind of the texture a reconciled stage already holds changes nothing
    // CE tracks: straight to the device, without the lock.
    if (DeviceState* known = FindExistingDevice(device);
        known && known->publication.IsSameTexture(static_cast<size_t>(samplerIndex), texture) &&
        known->configVersion.load(std::memory_order_relaxed) == GetActiveGraphicsConfigVersion()) {
        detail::CountRelaxed(known->pendingTextureBinds);
        return setTexture(device, stage, texture);
    }

    const ParsedSamplerConfig& parsed = detail::GetActiveParsedSamplerConfigCached();
    const bool overrideConfigured = parsed.overrideConfigured;
    DeviceState* deviceState = overrideConfigured ? FindOrCreateDevice(device) : FindExistingDevice(device);
    if (!deviceState || (!overrideConfigured && !deviceState->overrideActive.load(std::memory_order_acquire) &&
                         !deviceState->recording.load(std::memory_order_acquire))) {
        return setTexture(device, stage, texture);
    }

    ShadowLock lock(*deviceState, SamplerPublication::Bit(static_cast<size_t>(samplerIndex)));
    if (deviceState->recording.load(std::memory_order_relaxed)) {
        const HRESULT recordHr = setTexture(device, stage, texture);
        if (SUCCEEDED(recordHr)) {
            SamplerState recorded;
            detail::UpdateTextureMetadata(*deviceState, recorded, texture);
            RecordTexture(deviceState->recordingSnapshot, static_cast<size_t>(samplerIndex), recorded);
        }
        return recordHr;
    }
    RefreshConfigLocked(*deviceState, setState, getState);
    if (!overrideConfigured && !deviceState->overrideActive.load(std::memory_order_acquire))
        return setTexture(device, stage, texture);
    SamplerState& state = deviceState->samplers[static_cast<size_t>(samplerIndex)];
    if (!BootstrapSampler(*deviceState, stage, state, getState)) {
        return setTexture(device, stage, texture);
    }

    const HRESULT hr = setTexture(device, stage, texture);
    if (FAILED(hr)) {
        return hr;
    }

    if (!texture) {
        detail::UpdateTextureMetadata(*deviceState, state, nullptr);
        state.reconciled = false;
        return hr;
    }

    if (state.boundTexture == texture && state.reconciled) {
        return hr;
    }

    const StageTextureClassification prevClass{
        state.textureBound,
        state.textureMipLevels > 1,
        state.textureUsesAddressW,
        state.textureSupportsAnisotropy,
        state.textureSupportsMagAnisotropy};
    detail::UpdateTextureMetadata(*deviceState, state, texture);
    const StageTextureClassification nextClass{
        state.textureBound,
        state.textureMipLevels > 1,
        state.textureUsesAddressW,
        state.textureSupportsAnisotropy,
        state.textureSupportsMagAnisotropy};

    if (state.reconciled && prevClass == nextClass) {
        return hr;
    }

    if (!ReconcileSampler(device, stage, state, parsed, deviceState->maxAnisotropy, setState)) {
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
    ShadowLock lock(*deviceState, 0);
    RefreshConfigLocked(*deviceState, setState, getState);
}

void ReconcileAfterExternalStateChange(IDirect3DDevice9* device, const void* stateBlock, SetSamplerStateFn setState,
                                       GetSamplerStateFn getState) {
    if (!device || !setState || !getState)
        return;
    const ParsedSamplerConfig& parsed = detail::GetActiveParsedSamplerConfigCached();
    const bool overrideConfigured = parsed.overrideConfigured;
    DeviceState* deviceState = overrideConfigured ? FindOrCreateDevice(device) : FindExistingDevice(device);
    if (!deviceState)
        return;

    ShadowLock lock(*deviceState, SamplerPublication::kAllSamplers);
    // Nothing is forced and nothing is tracked: the block's state is the
    // application's, and there is no shadow to correct.
    if (deviceState->recording.load(std::memory_order_relaxed) ||
        (!overrideConfigured && !deviceState->overrideActive.load(std::memory_order_acquire)))
        return;

    const auto tracked = stateBlock ? deviceState->stateBlocks.find(stateBlock) : deviceState->stateBlocks.end();
    if (tracked != deviceState->stateBlocks.end()) {
        // Merge FIRST: a reconcile against the pre-Apply shadow is exactly the
        // write that used to undo the block.
        const uint32_t covered = ApplyStateBlockToShadow(*tracked->second, deviceState->samplers);
        bool reconciled = true;
        for (size_t i = 0; i < deviceState->samplers.size(); ++i) {
            if ((covered & (1u << i)) == 0)
                continue;
            SamplerState& sampler = deviceState->samplers[i];
            const DWORD samplerIndex = DenormalizeSampler(i);
            if (!BootstrapSampler(*deviceState, samplerIndex, sampler, getState))
                continue;
            reconciled =
                ReconcileSampler(device, samplerIndex, sampler, parsed, deviceState->maxAnisotropy, setState) && reconciled;
        }
        RefreshConfigLocked(*deviceState, setState, getState);
        if (!reconciled) {
            deviceState->configHash.store(0, std::memory_order_relaxed);
            deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
        }
        const uint64_t applies = g_trackedStateBlockApplies.fetch_add(1, std::memory_order_relaxed);
        if (applies < 4) {
            HookLogImportant("DX9: State block %p applied from its snapshot (samplers=0x%06X, no driver re-read)",
                             stateBlock, covered);
        }
        return;
    }

    bool complete = true;
    for (size_t i = 0; i < deviceState->samplers.size(); ++i) {
        SamplerState& sampler = deviceState->samplers[i];
        const bool wasInitialized = sampler.initialized;
        const SamplerStateValues previousPhysical = sampler.physical;
        complete = RefreshPhysicalSamplerState(*deviceState, DenormalizeSampler(i), sampler, getState) && complete;
        // An unseen block's values are the application's own: keep them.
        if (wasInitialized && sampler.initialized)
            AdoptExternalPhysical(sampler, previousPhysical);
    }
    deviceState->configHash.store(0, std::memory_order_relaxed);
    deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    RefreshConfigLocked(*deviceState, setState, getState);
    if (!complete) {
        deviceState->configHash.store(0, std::memory_order_relaxed);
        deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    }
    const uint64_t resyncs = g_externalResyncs.fetch_add(1, std::memory_order_relaxed);
    if (resyncs < 4) {
        HookLogImportant("DX9: State block %p applied without a snapshot (created before CE saw it); device re-read",
                         stateBlock);
    }
}

void InvalidateDevice(IDirect3DDevice9* device) {
    if (!device) {
        return;
    }
    DeviceState* deviceState = FindOrCreateDevice(device);
    ShadowLock lock(*deviceState, SamplerPublication::kAllSamplers);
    for (SamplerState& sampler : deviceState->samplers) {
        ResetSampler(sampler, false);
    }
    deviceState->configHash.store(0, std::memory_order_relaxed);
    deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    deviceState->overrideActive.store(false, std::memory_order_release);
    deviceState->textureMetaCache = {};
    detail::UpdateDeviceActiveOrRecordingCountLocked(*deviceState);
}

void ResetDevice(IDirect3DDevice9* device, bool intercepted) {
    if (!device) {
        return;
    }
    DeviceState* deviceState = FindOrCreateDevice(device);
    ShadowLock lock(*deviceState, SamplerPublication::kAllSamplers);
    const bool tracking = dx9_sampler_interception::ShadowTracksFromDefaults(
        intercepted, detail::GetActiveParsedSamplerConfigCached().overrideConfigured);
    for (SamplerState& sampler : deviceState->samplers) {
        ResetSampler(sampler, tracking);
    }
    // A classic device releases every state block before Reset; an Ex device's
    // survive it but restore pre-Reset values, so their snapshots are dropped
    // too and they take the re-read path.
    deviceState->stateBlocks.clear();
    deviceState->recording.store(false, std::memory_order_relaxed);
    deviceState->configHash.store(0, std::memory_order_relaxed);
    deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    deviceState->overrideActive.store(false, std::memory_order_release);
    deviceState->textureMetaCache = {};
    detail::UpdateDeviceActiveOrRecordingCountLocked(*deviceState);
}

void LogSummary() {
    // Device states are never freed, so the pointers outlive the registry lock;
    // each device's counters are read under its own mutex, never both at once.
    std::vector<DeviceState*> devices;
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        for (const auto& entry : g_devices)
            devices.push_back(entry.get());
    }
    uint64_t interceptedSets = 0;
    uint64_t redundantSets = 0;
    uint64_t textureMetadataQueries = 0;
    uint64_t textureMetadataCacheHits = 0;
    uint64_t publishedRedundantSets = 0;
    uint64_t publishedTextureBinds = 0;
    for (DeviceState* deviceState : devices) {
        // Holding it folds in what the publication answered without it.
        ShadowLock lock(*deviceState, 0);
        interceptedSets += deviceState->interceptedSets;
        redundantSets += deviceState->redundantSets;
        textureMetadataQueries += deviceState->textureMetadataQueries;
        textureMetadataCacheHits += deviceState->textureMetadataCacheHits;
        publishedRedundantSets += deviceState->publishedRedundantSets;
        publishedTextureBinds += deviceState->publishedTextureBinds;
    }
    HookLog(
        "DX9: Sampler override summary interceptedSets=%llu redundantSetsSkipped=%llu (lockFree=%llu) "
        "lockFreeTextureBinds=%llu textureMetadataQueries=%llu "
        "textureMetadataCacheHits=%llu creationTracked=%d reconciliations=%llu driverWrites=%llu companionWrites=%llu bootstrapQueries=%llu "
        "configChanges=%llu externalResyncs=%llu trackedStateBlockApplies=%llu recordedStateBlocks=%llu",
        static_cast<unsigned long long>(interceptedSets), static_cast<unsigned long long>(redundantSets),
        static_cast<unsigned long long>(publishedRedundantSets), static_cast<unsigned long long>(publishedTextureBinds),
        static_cast<unsigned long long>(textureMetadataQueries),
        static_cast<unsigned long long>(textureMetadataCacheHits),
        detail::TextureCreationTracked() ? 1 : 0,
        static_cast<unsigned long long>(g_reconciliations.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_driverWrites.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_companionWrites.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_bootstrapQueries.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_configChanges.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_externalResyncs.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_trackedStateBlockApplies.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(detail::RecordedStateBlockCount()));
}

}  // namespace ce::dx9_sampler_state
