#pragma once

// Shared between the legacy texture-stage shadow units:
// legacy_d3d_sampler_state.cpp (the shadow, Set/Get interception, the Apply
// reconcile), legacy_d3d_sampler_state_blocks.cpp (state-block recording and
// snapshots) and legacy_d3d_sampler_state_logging.cpp (decision lines).

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "hook/overrides/sampler_override/config_and_state.h"
#include "legacy_d3d_sampler_state.h"
#include "legacy_d3d_state_block_policy.h"

struct GraphicsConfig;

namespace ce::legacy_d3d_sampler_state::detail {

constexpr size_t kStageCount = legacy_d3d_state_block::kStageCount;
constexpr size_t kStateCount = legacy_d3d_state_block::kStateCount;

// Tracked state blocks per device; a game that keeps creating new handles is
// bounded here, and an evicted block falls back to the re-read path.
constexpr size_t kMaxTrackedStateBlocks = 2048;

struct StageState {
    std::array<DWORD, kStateCount> logical{};
    std::array<DWORD, kStateCount> physical{};
    uint64_t loggedMipFingerprint = ~uint64_t{0};
    int loggedAfDecision = -1;
    UINT loggedAfRequest = 0;
    bool loggedAfAggressive = false;
    bool initialized = false;
    bool bootstrapAttempted = false;
    // physical == the desired state for the current logical values and
    // configuration (legacy_d3d_state_block_policy.h, IsRedundantStageSet).
    bool reconciled = false;
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
    // Between BeginStateBlock and EndStateBlock the runtime records
    // SetTextureStageState calls instead of applying them; they go into
    // `recordingSnapshot`, not the shadow.
    std::atomic<bool> recording{false};
    // The recording began with the override hooks armed, so it sees all of its
    // Set* calls (hook_arming_policy.h); otherwise EndStateBlock keeps no
    // snapshot and the block re-reads the device on Apply.
    bool recordingTrusted = false;
    legacy_d3d_state_block::Snapshot recordingSnapshot;
    std::unordered_map<DWORD, legacy_d3d_state_block::Snapshot> stateBlocks;
    // SetTextureStageState calls that reached the shadow while an override was
    // active, and those answered without the policy or the driver
    // (IsRedundantStageSet). Plain counters guarded by `mutex`.
    uint64_t interceptedSets = 0;
    uint64_t redundantSets = 0;
};

struct MipMappingResult {
    sampler_override::LegacyD3DMipMappingDecision decision =
        sampler_override::LegacyD3DMipMappingDecision::OverrideDisabled;
    DWORD magFilter = 0;
    DWORD minFilter = 0;
    DWORD mipFilter = 0;
};

inline size_t ApiIndex(Api api) {
    if (api == Api::D3D6)
        return 0;
    return api == Api::D3D7 ? 1 : 2;
}

DeviceState* FindOrCreateDevice(Api api, void* device);
DeviceState* FindExistingDevice(Api api, void* device);
// True when SetTextureStageState currently keeps the shadow in step with the
// application (an override is configured or still active).
bool ShadowTracksApplication(const DeviceState& deviceState);
uint16_t TrackedStatesMask(Api api);
const char* ApiName(Api api);

// One line per stage when its mip / AF decision changes
// (legacy_d3d_sampler_state_logging.cpp).
void LogMipDecision(DeviceState& deviceState, DWORD stageIndex, StageState& state, const GraphicsConfig& gfx,
                    const MipMappingResult& result);
void LogAfDecision(DeviceState& deviceState, DWORD stageIndex, StageState& state, const GraphicsConfig& gfx,
                   sampler_override::LegacyD3DForcedAFDecision decision,
                   const std::array<DWORD, kStateCount>& desired);

}  // namespace ce::legacy_d3d_sampler_state::detail
