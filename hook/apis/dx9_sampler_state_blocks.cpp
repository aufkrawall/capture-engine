#include "dx9_sampler_state.h"

#include "dx9_sampler_state_internal.h"
#include "hook_common.h"

// State-block half of the D3D9 sampler shadow: what CreateStateBlock, Capture
// and BeginStateBlock/EndStateBlock recording leave behind for the Apply
// reconcile. The rules are in dx9_state_block_sampler_policy.h.

namespace ce::dx9_sampler_state {

using detail::DeviceState;
using detail::FindExistingDevice;
using detail::FindOrCreateDevice;

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

void OnBeginStateBlock(IDirect3DDevice9* device) {
    if (!device)
        return;
    DeviceState* deviceState = FindOrCreateDevice(device);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    deviceState->recordingSnapshot = StateBlockSnapshot{};
    deviceState->recording.store(true, std::memory_order_release);
}

void OnEndStateBlock(IDirect3DDevice9* device, const void* stateBlock) {
    if (!device)
        return;
    DeviceState* deviceState = FindOrCreateDevice(device);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    const bool wasRecording = deviceState->recording.exchange(false, std::memory_order_acq_rel);
    if (!wasRecording || !stateBlock)
        return;
    StoreStateBlockLocked(*deviceState, stateBlock, deviceState->recordingSnapshot);
    g_recordedStateBlocks.fetch_add(1, std::memory_order_relaxed);
}

void OnCreateStateBlock(IDirect3DDevice9* device, const void* stateBlock, D3DSTATEBLOCKTYPE type) {
    if (!device || !stateBlock)
        return;
    DeviceState* deviceState = FindOrCreateDevice(device);
    std::lock_guard<std::mutex> lock(deviceState->mutex);
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
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    const auto tracked = deviceState->stateBlocks.find(stateBlock);
    if (tracked != deviceState->stateBlocks.end())
        CaptureStateBlock(*tracked->second, deviceState->samplers);
}

void ForgetStateBlock(IDirect3DDevice9* device, const void* stateBlock) {
    DeviceState* deviceState = device ? FindExistingDevice(device) : nullptr;
    if (!deviceState)
        return;
    std::lock_guard<std::mutex> lock(deviceState->mutex);
    deviceState->stateBlocks.erase(stateBlock);
}

}  // namespace ce::dx9_sampler_state
