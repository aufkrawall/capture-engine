#include "dx9_sampler_state.h"

#include "dx9_sampler_state_internal.h"
#include "hook/runtime/hook_common.h"

// State-block half of the D3D9 sampler shadow: what CreateStateBlock, Capture
// and BeginStateBlock/EndStateBlock recording leave behind for the Apply
// reconcile. The rules are in dx9_state_block_sampler_policy.h.

namespace ce::dx9_sampler_state {

using detail::DeviceState;
using detail::FindExistingDevice;
using detail::FindOrCreateDevice;
using detail::ShadowLock;

namespace {

std::atomic<uint64_t> g_recordedStateBlocks{0};
std::atomic<int> g_stateBlockLogCount{0};

void StoreStateBlockLocked(DeviceState& deviceState, const void* block, const StateBlockSnapshot& snapshot) {
    auto existing = deviceState.stateBlocks.find(block);
    if (existing != deviceState.stateBlocks.end()) {
        *existing->second = snapshot;
        return;
    }
    if (deviceState.stateBlocks.size() >= detail::kMaxTrackedStateBlocks) {
        deviceState.stateBlocks.erase(deviceState.stateBlocks.begin());
        const int logIndex = g_stateBlockLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 4) {
            HookLogImportant("DX9: State-block tracking at its %zu-block cap; the evicted block re-reads the device "
                             "on Apply (#%d)",
                             detail::kMaxTrackedStateBlocks, logIndex + 1);
        }
    }
    deviceState.stateBlocks.emplace(block, std::make_unique<StateBlockSnapshot>(snapshot));
}

}  // namespace

namespace detail {

uint64_t RecordedStateBlockCount() {
    return g_recordedStateBlocks.load(std::memory_order_relaxed);
}

}  // namespace detail

void OnBeginStateBlock(IDirect3DDevice9* device, bool snapshotTrusted) {
    if (!device)
        return;
    DeviceState* deviceState = FindOrCreateDevice(device);
    ShadowLock lock(*deviceState, SamplerPublication::kAllSamplers);
    deviceState->recordingSnapshot = StateBlockSnapshot{};
    deviceState->recordingTrusted = snapshotTrusted;
    deviceState->recording.store(true, std::memory_order_release);
    detail::UpdateDeviceActiveOrRecordingCountLocked(*deviceState);
}

void OnEndStateBlock(IDirect3DDevice9* device, const void* stateBlock) {
    if (!device)
        return;
    DeviceState* deviceState = FindOrCreateDevice(device);
    ShadowLock lock(*deviceState, SamplerPublication::kAllSamplers);
    const bool wasRecording = deviceState->recording.exchange(false, std::memory_order_acq_rel);
    detail::UpdateDeviceActiveOrRecordingCountLocked(*deviceState);
    if (!wasRecording || !stateBlock)
        return;
    if (!deviceState->recordingTrusted) {
        // Begun before the per-draw slots were armed: some of its Set* calls
        // went past CE, so no snapshot - the block re-reads the device on Apply.
        const int logIndex = g_stateBlockLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 4) {
            HookLogImportant("DX9: State block %p was recorded before sampler interception was armed; it re-reads "
                             "the device on Apply (#%d)",
                             stateBlock, logIndex + 1);
        }
        return;
    }
    StoreStateBlockLocked(*deviceState, stateBlock, deviceState->recordingSnapshot);
    g_recordedStateBlocks.fetch_add(1, std::memory_order_relaxed);
}

void OnCreateStateBlock(IDirect3DDevice9* device, const void* stateBlock, D3DSTATEBLOCKTYPE type) {
    if (!device || !stateBlock)
        return;
    DeviceState* deviceState = FindOrCreateDevice(device);
    ShadowLock lock(*deviceState, 0);
    StateBlockSnapshot snapshot = MakeStateBlockSnapshot(CoverageForStateBlockType(type));
    CaptureStateBlock(snapshot, deviceState->samplers);
    StoreStateBlockLocked(*deviceState, stateBlock, snapshot);
}

void OnCaptureStateBlock(IDirect3DDevice9* device, const void* stateBlock) {
    if (!device || !stateBlock)
        return;
    DeviceState* deviceState = FindExistingDevice(device);
    if (!deviceState)
        return;
    ShadowLock lock(*deviceState, 0);
    const auto tracked = deviceState->stateBlocks.find(stateBlock);
    if (tracked != deviceState->stateBlocks.end())
        CaptureStateBlock(*tracked->second, deviceState->samplers);
}

void ForgetStateBlock(IDirect3DDevice9* device, const void* stateBlock) {
    DeviceState* deviceState = device ? FindExistingDevice(device) : nullptr;
    if (!deviceState)
        return;
    ShadowLock lock(*deviceState, 0);
    deviceState->stateBlocks.erase(stateBlock);
}

}  // namespace ce::dx9_sampler_state
