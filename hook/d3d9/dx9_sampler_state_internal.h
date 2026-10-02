#pragma once

// Shared between the sampler-shadow units: dx9_sampler_state.cpp (the shadow,
// the Set/Get interception, the Apply reconcile) and
// dx9_sampler_state_blocks.cpp (state-block recording and snapshots).

#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "dx9_state_block_sampler_policy.h"

namespace ce::dx9_sampler_state::detail {

// Tracked state blocks per device. A game that creates a block per frame and
// releases it reuses the address, which overwrites the entry; the cap bounds a
// game that keeps creating new ones. An evicted block falls back to the
// re-read path, which is conservative, never wrong in the physical view.
constexpr size_t kMaxTrackedStateBlocks = 2048;

struct DeviceState {
    IDirect3DDevice9* device = nullptr;
    std::mutex mutex;
    SamplerShadow samplers;
    // Between BeginStateBlock and EndStateBlock D3D9 records Set* calls
    // instead of applying them; they go into `recordingSnapshot`, not the shadow.
    std::atomic<bool> recording{false};
    StateBlockSnapshot recordingSnapshot;
    std::unordered_map<const void*, std::unique_ptr<StateBlockSnapshot>> stateBlocks;
    UINT maxAnisotropy = 1;
    DWORD textureFilterCaps = 0;
    DWORD cubeTextureFilterCaps = 0;
    DWORD volumeTextureFilterCaps = 0;
    std::atomic<uint64_t> configHash{0};
    std::atomic<uint32_t> configVersion{0xFFFFFFFFu};
    std::atomic<bool> overrideActive{false};
};

DeviceState* FindOrCreateDevice(IDirect3DDevice9* device);
DeviceState* FindExistingDevice(IDirect3DDevice9* device);

uint64_t RecordedStateBlockCount();

}  // namespace ce::dx9_sampler_state::detail
