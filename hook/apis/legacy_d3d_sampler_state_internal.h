#pragma once

// Shared between the legacy texture-stage shadow units:
// legacy_d3d_sampler_state.cpp (the shadow, Set/Get interception, the Apply
// reconcile) and legacy_d3d_sampler_state_blocks.cpp (state-block recording and
// snapshots).

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "legacy_d3d_sampler_state.h"
#include "legacy_d3d_state_block_policy.h"

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
    legacy_d3d_state_block::Snapshot recordingSnapshot;
    std::unordered_map<DWORD, legacy_d3d_state_block::Snapshot> stateBlocks;
};

DeviceState* FindOrCreateDevice(Api api, void* device);
DeviceState* FindExistingDevice(Api api, void* device);
// True when SetTextureStageState currently keeps the shadow in step with the
// application (an override is configured or still active).
bool ShadowTracksApplication(const DeviceState& deviceState);
uint16_t TrackedStatesMask(Api api);
const char* ApiName(Api api);

}  // namespace ce::legacy_d3d_sampler_state::detail
