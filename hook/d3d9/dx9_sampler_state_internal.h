#pragma once

// Shared between the sampler-shadow units: dx9_sampler_state.cpp (the shadow,
// the Set/Get interception, the Apply reconcile),
// dx9_sampler_state_blocks.cpp (state-block recording and snapshots) and
// dx9_sampler_state_texture_metadata.cpp (bound-texture metadata and its cache).

#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "dx9_sampler_config_policy.h"
#include "dx9_sampler_publication.h"
#include "dx9_state_block_sampler_policy.h"

namespace ce::dx9_sampler_state::detail {

// Tracked state blocks per device. A game that creates a block per frame and
// releases it reuses the address, which overwrites the entry; the cap bounds a
// game that keeps creating new ones. An evicted block falls back to the
// re-read path, which is conservative, never wrong in the physical view.
constexpr size_t kMaxTrackedStateBlocks = 2048;

struct DeviceState {
    IDirect3DDevice9* device = nullptr;
    // Held by every change to `samplers`, always through ShadowLock.
    std::mutex mutex;
    SamplerShadow samplers;
    // The shadow's lock-free answers (dx9_sampler_publication.h), and the
    // samplers the current ShadowLock holder withdrew from it.
    SamplerPublication publication;
    uint32_t withdrawn = 0;
    // Between BeginStateBlock and EndStateBlock D3D9 records Set* calls
    // instead of applying them; they go into `recordingSnapshot`, not the shadow.
    std::atomic<bool> recording{false};
    // The recording began with the per-draw slots armed, so it sees all of its
    // Set* calls (dx9_sampler_interception_policy.h); otherwise EndStateBlock
    // keeps no snapshot and the block re-reads the device on Apply.
    bool recordingTrusted = false;
    StateBlockSnapshot recordingSnapshot;
    std::unordered_map<const void*, std::unique_ptr<StateBlockSnapshot>> stateBlocks;
    UINT maxAnisotropy = 1;
    DWORD textureFilterCaps = 0;
    DWORD cubeTextureFilterCaps = 0;
    DWORD volumeTextureFilterCaps = 0;
    std::atomic<uint64_t> configHash{0};
    std::atomic<uint32_t> configVersion{0xFFFFFFFFu};
    std::atomic<bool> overrideActive{false};
    ParsedSamplerConfig parsedConfig{};
    bool countedAsActiveOrRecording = false;
    TextureMetaCache textureMetaCache{};
    // SetSamplerState calls that reached the shadow while an override was
    // active, those answered without the policy or the driver
    // (IsRedundantLogicalSet), and texture binds that had to ask the texture
    // over COM vs. those the creation-tracked cache answered. Plain counters:
    // guarded by `mutex`. What the publication answered without the lock -
    // redundant sets, and re-binds of a stage's texture - is counted in the
    // pending counters with a plain load and store (two threads counting at
    // once can lose one) and folded in by the next lock holder.
    uint64_t interceptedSets = 0;
    uint64_t redundantSets = 0;
    uint64_t textureMetadataQueries = 0;
    uint64_t textureMetadataCacheHits = 0;
    uint64_t publishedRedundantSets = 0;
    uint64_t publishedTextureBinds = 0;
    std::atomic<uint32_t> pendingRedundantSets{0};
    std::atomic<uint32_t> pendingTextureBinds{0};
};

extern std::atomic<uint32_t> g_activeOrRecordingDeviceCount;
const ParsedSamplerConfig& GetActiveParsedSamplerConfigCached();
void UpdateDeviceActiveOrRecordingCountLocked(DeviceState& deviceState);

// Called by ShadowLock's holder BEFORE it changes samplers beyond those it
// named: they leave the publication until the lock is released.
inline void WithdrawLocked(DeviceState& deviceState, uint32_t samplers) {
    deviceState.publication.Withdraw(samplers);
    deviceState.withdrawn |= samplers;
}

// Republishes what the holder withdrew and ended clean, and folds in the
// lock-free counts (dx9_sampler_state.cpp).
void RepublishLocked(DeviceState& deviceState);

// The device mutex. Entry withdraws `samplers` from the publication, which is
// everything the holder may change; release republishes the clean ones. A
// holder that turns out to change more calls WithdrawLocked first.
class ShadowLock {
public:
    ShadowLock(DeviceState& deviceState, uint32_t samplers) : deviceState_(deviceState), lock_(deviceState.mutex) {
        WithdrawLocked(deviceState_, samplers);
    }
    ~ShadowLock() {
        RepublishLocked(deviceState_);
    }
    ShadowLock(const ShadowLock&) = delete;
    ShadowLock& operator=(const ShadowLock&) = delete;

private:
    DeviceState& deviceState_;
    std::lock_guard<std::mutex> lock_;
};

inline void CountRelaxed(std::atomic<uint32_t>& counter) {
    counter.store(counter.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
}

DeviceState* FindOrCreateDevice(IDirect3DDevice9* device);
DeviceState* FindExistingDevice(IDirect3DDevice9* device);

inline bool FilterCapsAllowMinAnisotropy(DWORD caps) {
    return (caps & D3DPTFILTERCAPS_MINFANISOTROPIC) != 0;
}

inline bool FilterCapsAllowMagAnisotropy(DWORD caps) {
    return (caps & D3DPTFILTERCAPS_MAGFANISOTROPIC) != 0;
}

// The bound texture's level count, type and filter support into `state`
// (dx9_sampler_state_texture_metadata.cpp). The device mutex is held.
void UpdateTextureMetadata(DeviceState& deviceState, SamplerState& state, IDirect3DBaseTexture9* texture);
bool TextureCreationTracked();
// Every device's stages re-derive their texture's metadata on the next
// SetTexture, also of the texture already bound.
void ForgetBoundTextures();

uint64_t RecordedStateBlockCount();

}  // namespace ce::dx9_sampler_state::detail
