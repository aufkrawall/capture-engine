#include "legacy_d3d_sampler_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <mutex>
#include <vector>

#include "../common/sampler_override_utils.h"
#include "hook_common.h"
#include "lod_helper.h"

namespace ce::legacy_d3d_sampler_state {
namespace {

constexpr size_t kStageCount = 8;
constexpr size_t kStateCount = 9;
constexpr std::array<DWORD, kStateCount> kTrackedTypes = {13, 14, 25, 16, 17, 18, 19, 20, 21};

struct StageState {
    std::array<DWORD, kStateCount> logical{};
    std::array<DWORD, kStateCount> physical{};
    bool initialized = false;
    bool bootstrapAttempted = false;
};

struct DeviceState {
    Api api = Api::D3D8;
    void* device = nullptr;
    std::mutex mutex;
    std::array<StageState, kStageCount> stages;
    UINT maxAnisotropy = 1;
    std::atomic<uint64_t> configHash{0};
    std::atomic<uint32_t> configVersion{0xFFFFFFFFu};
    std::atomic<bool> overrideActive{false};
    bool bootstrapSweepPending = true;
};

std::mutex g_registryMutex;
std::vector<std::unique_ptr<DeviceState>> g_devices;
std::array<std::atomic<uint64_t>, 3> g_reconciliations{};
std::array<std::atomic<uint64_t>, 3> g_driverWrites{};
std::array<std::atomic<uint64_t>, 3> g_bootstraps{};
std::array<std::atomic<int>, 3> g_transitionLogs{};
std::array<std::atomic<int>, 3> g_failureLogs{};

thread_local Api t_cachedApi = Api::D3D8;
thread_local void* t_cachedDevice = nullptr;
thread_local DeviceState* t_cachedState = nullptr;

size_t ApiIndex(Api api) {
    if (api == Api::D3D6)
        return 0;
    return api == Api::D3D7 ? 1 : 2;
}

const char* ApiName(Api api) {
    if (api == Api::D3D6)
        return "DX6";
    return api == Api::D3D7 ? "DX7" : "DX8";
}

sampler_override::LegacyD3DSamplerTraits TraitsFor(Api api) {
    sampler_override::LegacyD3DSamplerTraits traits = {};
    if (api != Api::D3D8) {
        traits.anisotropicMag = 5;  // D3DTFG_ANISOTROPIC differs from D3DTFN_ANISOTROPIC.
        traits.mipNone = 1;
        traits.mipPoint = 2;
        traits.mipLinear = 3;
    }
    return traits;
}

void ResetStage(StageState& state, Api api, bool defaultsAreKnown) {
    const auto traits = TraitsFor(api);
    state = {};
    state.logical = {1, 1, 1, traits.pointMag, traits.pointMin, traits.mipNone, 0, 0, 1};
    state.physical = state.logical;
    state.initialized = defaultsAreKnown;
    state.bootstrapAttempted = defaultsAreKnown;
}

DeviceState* FindOrCreate(Api api, void* device, QueryMaxAnisotropyFn queryMaxAnisotropy) {
    if (t_cachedApi == api && t_cachedDevice == device && t_cachedState) {
        return t_cachedState;
    }

    std::lock_guard<std::mutex> lock(g_registryMutex);
    for (const auto& entry : g_devices) {
        if (entry->api == api && entry->device == device) {
            t_cachedApi = api;
            t_cachedDevice = device;
            t_cachedState = entry.get();
            return entry.get();
        }
    }

    auto entry = std::make_unique<DeviceState>();
    entry->api = api;
    entry->device = device;
    entry->maxAnisotropy = queryMaxAnisotropy ? std::max<UINT>(1, queryMaxAnisotropy(device)) : 1;
    for (StageState& stage : entry->stages) {
        ResetStage(stage, api, false);
    }
    DeviceState* result = entry.get();
    g_devices.push_back(std::move(entry));
    t_cachedApi = api;
    t_cachedDevice = device;
    t_cachedState = result;
    return result;
}

DeviceState* FindExisting(Api api, void* device) {
    if (t_cachedApi == api && t_cachedDevice == device)
        return t_cachedState;

    std::lock_guard<std::mutex> lock(g_registryMutex);
    for (const auto& entry : g_devices) {
        if (entry->api == api && entry->device == device) {
            t_cachedApi = api;
            t_cachedDevice = device;
            t_cachedState = entry.get();
            return entry.get();
        }
    }
    return nullptr;
}

int StateIndex(DWORD type) {
    for (size_t i = 0; i < kTrackedTypes.size(); ++i) {
        if (kTrackedTypes[i] == type)
            return static_cast<int>(i);
    }
    return -1;
}

bool HasOverride(const GraphicsConfig& gfx) {
    return (!gfx.anisotropicFiltering.empty() && gfx.anisotropicFiltering != "default") ||
           (!gfx.mipMapping.empty() && gfx.mipMapping != "default") || HasConfiguredMipBias(gfx) ||
           gfx.forceMipBiasClamp || (gfx.sgssaa && !gfx.disableAutoMipBias);
}

bool Bootstrap(DeviceState& deviceState, DWORD stage, StageState& state, GetTextureStageStateFn getState) {
    if (state.initialized)
        return true;
    if (state.bootstrapAttempted || !getState)
        return false;
    state.bootstrapAttempted = true;

    bool succeeded = true;
    for (size_t i = 0; i < kTrackedTypes.size(); ++i) {
        if (i == 2 && deviceState.api != Api::D3D8)
            continue;
        DWORD value = state.logical[i];
        if (SUCCEEDED(getState(deviceState.device, stage, kTrackedTypes[i], &value))) {
            state.logical[i] = value;
            state.physical[i] = value;
        } else {
            succeeded = false;
        }
    }
    state.initialized = succeeded;
    g_bootstraps[ApiIndex(deviceState.api)].fetch_add(1, std::memory_order_relaxed);
    return succeeded;
}

std::array<DWORD, kStateCount> BuildDesired(const DeviceState& deviceState, const StageState& state,
                                            const GraphicsConfig& gfx,
                                            sampler_override::LegacyD3DForcedAFDecision* decision) {
    const auto traits = TraitsFor(deviceState.api);
    std::array<DWORD, kStateCount> desired = state.logical;
    const auto materialAddress = [](DWORD address) { return address >= 1 && address <= 3; };
    const bool safeAddress =
        gfx.samplerOverrideMode == "aggressive" ||
        (materialAddress(desired[0]) && materialAddress(desired[1]) && materialAddress(desired[2]));

    if (desired[5] != traits.mipNone && safeAddress) {
        if (gfx.mipMapping == "trilinear") {
            desired[3] = traits.linearMag;
            desired[4] = traits.linearMin;
            desired[5] = traits.mipLinear;
        } else if (gfx.mipMapping == "bilinear") {
            desired[3] = traits.linearMag;
            desired[4] = traits.linearMin;
            desired[5] = traits.mipPoint;
        } else if (gfx.mipMapping == "nearest") {
            desired[3] = traits.pointMag;
            desired[4] = traits.pointMin;
            desired[5] = traits.mipPoint;
        }
    }

    sampler_override::LegacyD3DSamplerForcedAFInfo info = {};
    info.addressU = desired[0];
    info.addressV = desired[1];
    info.addressW = desired[2];
    info.magFilter = desired[3];
    info.minFilter = desired[4];
    info.mipFilter = desired[5];
    info.deviceMaxAnisotropy = deviceState.maxAnisotropy;
    const auto afDecision = sampler_override::ClassifyLegacyD3DSamplerForForcedAF(info, traits, gfx);
    if (decision)
        *decision = afDecision;
    if (afDecision == sampler_override::LegacyD3DForcedAFDecision::Allow) {
        desired[3] = traits.anisotropicMag;
        desired[4] = traits.anisotropicMin;
        desired[8] = sampler_override::ResolveLegacyD3DForcedAnisotropy(info, gfx);
    } else if (gfx.anisotropicFiltering == "off") {
        if (desired[3] == traits.anisotropicMag)
            desired[3] = traits.linearMag;
        if (desired[4] == traits.anisotropicMin)
            desired[4] = traits.linearMin;
        desired[8] = 1;
    }

    float bias = ApplyConfiguredMipBias(gfx, std::bit_cast<float>(state.logical[6]));
    if (gfx.sgssaa && !gfx.disableAutoMipBias && !gfx.forceMipBiasClamp) {
        float sgssaaBias = 0.0f;
        if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgssaaBias))
            bias += sgssaaBias;
    }
    desired[6] = std::bit_cast<DWORD>(FinalizeMipBias(gfx, bias));
    return desired;
}

bool WriteCompanions(DeviceState& deviceState, DWORD stage, StageState& state,
                     const std::array<DWORD, kStateCount>& desired, int requestedIndex,
                     SetTextureStageStateFn setState) {
    if (!setState)
        return false;
    const size_t apiIndex = ApiIndex(deviceState.api);
    bool succeeded = true;
    for (size_t i = 0; i < kTrackedTypes.size(); ++i) {
        if (i == 2 && deviceState.api != Api::D3D8)
            continue;
        if (static_cast<int>(i) == requestedIndex || state.physical[i] == desired[i])
            continue;
        const HRESULT hr = setState(deviceState.device, stage, kTrackedTypes[i], desired[i]);
        if (SUCCEEDED(hr)) {
            state.physical[i] = desired[i];
            g_driverWrites[apiIndex].fetch_add(1, std::memory_order_relaxed);
        } else {
            succeeded = false;
            const int logIndex = g_failureLogs[apiIndex].fetch_add(1, std::memory_order_relaxed);
            if (logIndex < 8) {
                HookLogImportant("%s: Sampler companion write failed stage=%u type=%u value=%u hr=0x%08X (#%d)",
                                 ApiName(deviceState.api), stage, kTrackedTypes[i], desired[i], hr, logIndex + 1);
            }
        }
    }
    return succeeded;
}

bool ReconcileStage(DeviceState& deviceState, DWORD stageIndex, StageState& state, const GraphicsConfig& gfx,
                    SetTextureStageStateFn setState) {
    sampler_override::LegacyD3DForcedAFDecision decision =
        sampler_override::LegacyD3DForcedAFDecision::OverrideDisabled;
    const auto desired = BuildDesired(deviceState, state, gfx, &decision);
    if (desired == state.physical)
        return true;

    const bool succeeded = WriteCompanions(deviceState, stageIndex, state, desired, -1, setState);
    g_reconciliations[ApiIndex(deviceState.api)].fetch_add(1, std::memory_order_relaxed);
    const int logIndex = g_transitionLogs[ApiIndex(deviceState.api)].fetch_add(1, std::memory_order_relaxed);
    if (logIndex < 24) {
        HookLogImportant("%s: Event-driven sampler reconcile stage=%u decision=%d aniso=%u policy=%s (#%d)",
                         ApiName(deviceState.api), stageIndex, static_cast<int>(decision), desired[8],
                         gfx.samplerOverrideMode.c_str(), logIndex + 1);
    }
    return succeeded && state.physical == desired;
}

void RefreshConfigLocked(DeviceState& deviceState, SetTextureStageStateFn setState, GetTextureStageStateFn getState,
                         bool sweepUnknownStages) {
    const uint32_t version = GetActiveGraphicsConfigVersion();
    const bool sweepNeeded = sweepUnknownStages && deviceState.bootstrapSweepPending &&
                             deviceState.overrideActive.load(std::memory_order_relaxed);
    if (deviceState.configVersion.load(std::memory_order_relaxed) == version &&
        deviceState.configHash.load(std::memory_order_relaxed) != 0 && !sweepNeeded) {
        return;
    }

    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    const uint64_t hash = sampler_override::HashSamplerOverrideConfig(gfx);
    const bool active = HasOverride(gfx);
    const bool needsActiveSweep = active && sweepUnknownStages && deviceState.bootstrapSweepPending;
    if (deviceState.configHash.load(std::memory_order_relaxed) == hash &&
        deviceState.overrideActive.load(std::memory_order_relaxed) == active && !needsActiveSweep) {
        deviceState.configVersion.store(version, std::memory_order_release);
        return;
    }

    bool complete = true;
    for (size_t i = 0; i < deviceState.stages.size(); ++i) {
        StageState& stage = deviceState.stages[i];
        if (needsActiveSweep && !stage.initialized)
            Bootstrap(deviceState, static_cast<DWORD>(i), stage, getState);
        if (!stage.initialized) {
            if (!active)
                ResetStage(stage, deviceState.api, false);
            continue;
        }
        complete = ReconcileStage(deviceState, static_cast<DWORD>(i), stage, gfx, setState) && complete;
        if (!active && stage.physical == stage.logical)
            ResetStage(stage, deviceState.api, false);
    }
    if (needsActiveSweep)
        deviceState.bootstrapSweepPending = false;
    else if (!active)
        deviceState.bootstrapSweepPending = true;

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

void RegisterDevice(Api api, void* device, bool newDevice, QueryMaxAnisotropyFn queryMaxAnisotropy) {
    if (!device)
        return;
    DeviceState* deviceState = FindOrCreate(api, device, queryMaxAnisotropy);
    if (!newDevice)
        return;

    std::lock_guard<std::mutex> lock(deviceState->mutex);
    deviceState->maxAnisotropy = queryMaxAnisotropy ? std::max<UINT>(1, queryMaxAnisotropy(device)) : 1;
    for (StageState& stage : deviceState->stages)
        ResetStage(stage, api, true);
    deviceState->configHash.store(0, std::memory_order_relaxed);
    deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    deviceState->overrideActive.store(false, std::memory_order_release);
    deviceState->bootstrapSweepPending = false;
}

HRESULT SetTextureStageState(Api api, void* device, DWORD stage, DWORD type, DWORD value,
                             SetTextureStageStateFn setState, GetTextureStageStateFn getState,
                             QueryMaxAnisotropyFn queryMaxAnisotropy) {
    const bool combinedAddress = api != Api::D3D8 && type == 12;
    const int stateIndex = StateIndex(type);
    if (!device || !setState || stage >= kStageCount || (api != Api::D3D8 && type == 25) ||
        (stateIndex < 0 && !combinedAddress))
        return setState ? setState(device, stage, type, value) : E_INVALIDARG;

    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    const bool overrideConfigured = HasOverride(gfx);
    DeviceState* deviceState =
        overrideConfigured ? FindOrCreate(api, device, queryMaxAnisotropy) : FindExisting(api, device);
    if (!deviceState || (!overrideConfigured && !deviceState->overrideActive.load(std::memory_order_acquire)))
        return setState(device, stage, type, value);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    RefreshConfigLocked(*deviceState, setState, getState, false);
    if (!overrideConfigured && !deviceState->overrideActive.load(std::memory_order_acquire))
        return setState(device, stage, type, value);
    StageState& state = deviceState->stages[stage];
    if (!Bootstrap(*deviceState, stage, state, getState))
        return setState(device, stage, type, value);
    const auto previousLogical = state.logical;
    if (combinedAddress) {
        state.logical[0] = value;
        state.logical[1] = value;
    } else {
        state.logical[static_cast<size_t>(stateIndex)] = value;
    }

    sampler_override::LegacyD3DForcedAFDecision decision =
        sampler_override::LegacyD3DForcedAFDecision::OverrideDisabled;
    const auto desired = BuildDesired(*deviceState, state, gfx, &decision);
    const DWORD requestedValue = combinedAddress ? value : desired[static_cast<size_t>(stateIndex)];
    const HRESULT hr = setState(device, stage, type, requestedValue);
    if (SUCCEEDED(hr)) {
        if (combinedAddress) {
            state.physical[0] = value;
            state.physical[1] = value;
        } else {
            state.physical[static_cast<size_t>(stateIndex)] = desired[static_cast<size_t>(stateIndex)];
        }
        const bool companionsSucceeded =
            WriteCompanions(*deviceState, stage, state, desired, combinedAddress ? -1 : stateIndex, setState);
        if (!companionsSucceeded) {
            deviceState->configHash.store(0, std::memory_order_relaxed);
            deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
        }
        g_driverWrites[ApiIndex(api)].fetch_add(1, std::memory_order_relaxed);
    } else {
        state.logical = previousLogical;
    }
    if (SUCCEEDED(hr) && desired != state.logical) {
        g_reconciliations[ApiIndex(api)].fetch_add(1, std::memory_order_relaxed);
        const int logIndex = g_transitionLogs[ApiIndex(api)].fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 24) {
            HookLogImportant("%s: Event-driven sampler reconcile stage=%u decision=%d aniso=%u policy=%s (#%d)",
                             ApiName(api), stage, static_cast<int>(decision), desired[8],
                             gfx.samplerOverrideMode.c_str(), logIndex + 1);
        }
    }
    return hr;
}

HRESULT GetTextureStageState(Api api, void* device, DWORD stage, DWORD type, DWORD* value,
                             GetTextureStageStateFn getState, SetTextureStageStateFn setState,
                             QueryMaxAnisotropyFn queryMaxAnisotropy) {
    const bool combinedAddress = api != Api::D3D8 && type == 12;
    const int stateIndex = StateIndex(type);
    if (!device || !getState || !value || stage >= kStageCount || (api != Api::D3D8 && type == 25) ||
        (stateIndex < 0 && !combinedAddress))
        return getState ? getState(device, stage, type, value) : E_INVALIDARG;
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    const bool overrideConfigured = HasOverride(gfx);
    DeviceState* deviceState =
        overrideConfigured ? FindOrCreate(api, device, queryMaxAnisotropy) : FindExisting(api, device);
    if (!deviceState || (!overrideConfigured && !deviceState->overrideActive.load(std::memory_order_acquire)))
        return getState(device, stage, type, value);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    RefreshConfigLocked(*deviceState, setState, getState, false);
    if (!deviceState->overrideActive.load(std::memory_order_acquire))
        return getState(device, stage, type, value);
    StageState& state = deviceState->stages[stage];
    if (!Bootstrap(*deviceState, stage, state, getState))
        return getState(device, stage, type, value);
    if (!ReconcileStage(*deviceState, stage, state, gfx, setState)) {
        deviceState->configHash.store(0, std::memory_order_relaxed);
        deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    }
    *value = combinedAddress ? state.logical[0] : state.logical[static_cast<size_t>(stateIndex)];
    return S_OK;
}

void RefreshConfiguration(Api api, void* device, SetTextureStageStateFn setState, GetTextureStageStateFn getState,
                          QueryMaxAnisotropyFn queryMaxAnisotropy) {
    if (!device || !setState)
        return;
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    const bool overrideConfigured = HasOverride(gfx);
    DeviceState* deviceState =
        overrideConfigured ? FindOrCreate(api, device, queryMaxAnisotropy) : FindExisting(api, device);
    if (!deviceState)
        return;
    const uint32_t version = GetActiveGraphicsConfigVersion();
    const bool sweepNeeded = overrideConfigured && deviceState->bootstrapSweepPending;
    if (deviceState->configVersion.load(std::memory_order_acquire) == version &&
        deviceState->configHash.load(std::memory_order_relaxed) != 0 && !sweepNeeded) {
        return;
    }
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    RefreshConfigLocked(*deviceState, setState, getState, true);
}

void ResetDevice(Api api, void* device) {
    if (!device)
        return;
    DeviceState* deviceState = FindOrCreate(api, device, nullptr);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    for (StageState& stage : deviceState->stages)
        ResetStage(stage, api, true);
    deviceState->configHash.store(0, std::memory_order_relaxed);
    deviceState->configVersion.store(0xFFFFFFFFu, std::memory_order_release);
    deviceState->overrideActive.store(false, std::memory_order_release);
    deviceState->bootstrapSweepPending = false;
}

void LogSummary(Api api) {
    const size_t index = ApiIndex(api);
    HookLog("%s: Sampler override summary reconciliations=%llu driverWrites=%llu bootstraps=%llu", ApiName(api),
            static_cast<unsigned long long>(g_reconciliations[index].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_driverWrites[index].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_bootstraps[index].load(std::memory_order_relaxed)));
}

}  // namespace ce::legacy_d3d_sampler_state
