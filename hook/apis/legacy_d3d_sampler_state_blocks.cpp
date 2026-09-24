#include "legacy_d3d_sampler_state.h"

#include "hook_common.h"
#include "legacy_d3d_sampler_state_internal.h"

// State-block half of the legacy (D3D7/D3D8) texture-stage shadow: what
// CreateStateBlock, CaptureStateBlock and BeginStateBlock/EndStateBlock recording
// leave behind for the Apply reconcile. The rules are in
// legacy_d3d_state_block_policy.h.

namespace ce::legacy_d3d_sampler_state {

using detail::DeviceState;
using detail::FindExistingDevice;
using detail::FindOrCreateDevice;
namespace blocks = ce::legacy_d3d_state_block;

namespace {

std::atomic<int> g_stateBlockLogCount{0};

void StoreStateBlockLocked(DeviceState& deviceState, DWORD handle, const blocks::Snapshot& snapshot) {
    auto existing = deviceState.stateBlocks.find(handle);
    if (existing != deviceState.stateBlocks.end()) {
        existing->second = snapshot;
        return;
    }
    if (deviceState.stateBlocks.size() >= detail::kMaxTrackedStateBlocks) {
        deviceState.stateBlocks.erase(deviceState.stateBlocks.begin());
        const int logIndex = g_stateBlockLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 4) {
            HookLogImportant("%s: State-block tracking at its %zu-block cap; the evicted block re-reads the device "
                             "on Apply (#%d)",
                             detail::ApiName(deviceState.api), detail::kMaxTrackedStateBlocks, logIndex + 1);
        }
    }
    deviceState.stateBlocks.emplace(handle, snapshot);
}

}  // namespace

void OnBeginStateBlock(Api api, void* device) {
    if (!device)
        return;
    DeviceState* deviceState = FindOrCreateDevice(api, device);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    deviceState->recordingSnapshot = blocks::Snapshot{};
    deviceState->recording.store(true, std::memory_order_release);
}

void OnEndStateBlock(Api api, void* device, bool succeeded, DWORD handle) {
    if (!device)
        return;
    DeviceState* deviceState = FindExistingDevice(api, device);
    if (!deviceState)
        return;
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    const bool wasRecording = deviceState->recording.exchange(false, std::memory_order_acq_rel);
    if (!wasRecording || !succeeded)
        return;
    StoreStateBlockLocked(*deviceState, handle, deviceState->recordingSnapshot);
}

void OnCreateStateBlock(Api api, void* device, DWORD type, DWORD handle) {
    if (!device)
        return;
    DeviceState* deviceState = FindOrCreateDevice(api, device);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    blocks::Snapshot snapshot = blocks::MakeSnapshot(type, detail::TrackedStatesMask(api));
    blocks::CaptureSnapshot(snapshot, deviceState->stages, detail::ShadowTracksApplication(*deviceState));
    StoreStateBlockLocked(*deviceState, handle, snapshot);
}

void OnCaptureStateBlock(Api api, void* device, DWORD handle) {
    if (!device)
        return;
    DeviceState* deviceState = FindExistingDevice(api, device);
    if (!deviceState)
        return;
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    const auto tracked = deviceState->stateBlocks.find(handle);
    if (tracked != deviceState->stateBlocks.end())
        blocks::CaptureSnapshot(tracked->second, deviceState->stages, detail::ShadowTracksApplication(*deviceState));
}

void ForgetStateBlock(Api api, void* device, DWORD handle) {
    DeviceState* deviceState = device ? FindExistingDevice(api, device) : nullptr;
    if (!deviceState)
        return;
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    deviceState->stateBlocks.erase(handle);
}

}  // namespace ce::legacy_d3d_sampler_state
