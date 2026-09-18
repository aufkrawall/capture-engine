#pragma once

// Session-scoped render-endpoint latency channel.
//
// The render->loopback A/V latency probe (mediaengine/audio_latency_probe.{h,cpp}) costs about
// 3.2 s per measurement and its result is cached only in process memory. That was designed as
// "one probe per fresh CE process", but the media process is DISPOSABLE: the controller spawns it
// on the recording hotkey and it exits after every stop to release GPU VRAM. The process-memory
// cache therefore never hits, and every single recording paid the full probe on the start path -
// which is why a recording shorter than the startup latency produced no output at all.
//
// This channel restores the intended "once per CE session" behaviour without reintroducing the
// deliberately removed `audio_latency_cache.ini`: the controller owns a small anonymous file
// mapping for its whole lifetime and each disposable media child inherits a handle to it. The
// child reads the measured value before probing and writes a fresh measurement back, so the next
// child starts warm. Nothing is written to disk and the endpoint key never reaches a command line
// or any other process.
//
// The layout below is the cross-process contract. The pure table logic is header-only so it is
// unit-testable without mapping anything; the Win32 create/open/map wrappers live in the .cpp.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace ce::av_sync {

inline constexpr uint32_t kLatencyChannelMagic = 0x43415641;  // "AVAC"
inline constexpr uint32_t kLatencyChannelVersion = 1;
// The endpoint key folds in the device id plus format/period metadata; observed keys are ~120
// chars. The capacity is fixed so the block stays a trivially copyable POD.
inline constexpr size_t kLatencyChannelKeyCapacity = 256;
// One entry per distinct render-endpoint configuration seen during a session. A handful covers
// device switches and format changes; the oldest entry is replaced once full.
inline constexpr size_t kLatencyChannelMaxEntries = 4;

struct LatencyChannelEntry {
    char key[kLatencyChannelKeyCapacity];
    double latencyMs;
};

// Written by whichever disposable media child measured last, read by the next one. Writers are
// serialized by construction (the controller runs at most one media child at a time) but the
// sequence counter still makes a torn read impossible to mistake for data.
struct LatencyChannelBlock {
    uint32_t magic;
    uint32_t version;
    uint32_t structSize;
    uint32_t entryCount;
    std::atomic<uint64_t> sequence;  // even = stable, odd = write in progress
    LatencyChannelEntry entries[kLatencyChannelMaxEntries];
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "the latency channel sequence must be lock-free to be shared across processes");

// ---- Pure table logic (unit-tested; no Windows dependency) ------------------------------------

inline void InitLatencyChannel(LatencyChannelBlock& block) {
    std::memset(block.entries, 0, sizeof(block.entries));
    block.magic = kLatencyChannelMagic;
    block.version = kLatencyChannelVersion;
    block.structSize = static_cast<uint32_t>(sizeof(LatencyChannelBlock));
    block.entryCount = 0;
    block.sequence.store(0, std::memory_order_release);
}

inline bool IsLatencyChannelCompatible(const LatencyChannelBlock& block) {
    return block.magic == kLatencyChannelMagic && block.version == kLatencyChannelVersion &&
           block.structSize == static_cast<uint32_t>(sizeof(LatencyChannelBlock)) &&
           block.entryCount <= kLatencyChannelMaxEntries;
}

// A key that cannot round-trip through the fixed-capacity entry is refused rather than truncated:
// a truncated key could collide with a different endpoint and hand back the wrong latency.
inline bool LatencyChannelKeyFits(const std::string& key) {
    return !key.empty() && key.size() < kLatencyChannelKeyCapacity;
}

// Reads the entry for `key`. Returns false on an incompatible block, an unusable key, a miss, or a
// concurrent write (the caller then measures, which is always safe).
inline bool LookupLatencyChannel(const LatencyChannelBlock& block, const std::string& key, double* outMs) {
    if (!LatencyChannelKeyFits(key)) {
        return false;
    }
    const uint64_t before = block.sequence.load(std::memory_order_acquire);
    if ((before & 1ULL) != 0ULL || !IsLatencyChannelCompatible(block)) {
        return false;
    }
    double found = 0.0;
    bool hit = false;
    for (uint32_t i = 0; i < block.entryCount && i < kLatencyChannelMaxEntries; ++i) {
        const LatencyChannelEntry& entry = block.entries[i];
        // The writer always NUL-terminates; guard anyway so a corrupt block cannot run off the end.
        if (entry.key[kLatencyChannelKeyCapacity - 1] != '\0') {
            continue;
        }
        if (key == entry.key) {
            found = entry.latencyMs;
            hit = true;
            break;
        }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (block.sequence.load(std::memory_order_acquire) != before) {
        return false;  // raced with a write; treat as a miss
    }
    if (hit && outMs) {
        *outMs = found;
    }
    return hit;
}

// Inserts or updates `key`. Returns false when the key cannot be stored intact or the block is not
// a compatible channel. Storing is best-effort: a failure only costs the next child a re-probe.
inline bool UpsertLatencyChannel(LatencyChannelBlock& block, const std::string& key, double latencyMs) {
    if (!LatencyChannelKeyFits(key) || !IsLatencyChannelCompatible(block)) {
        return false;
    }
    block.sequence.fetch_add(1, std::memory_order_acq_rel);  // -> odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);

    uint32_t slot = kLatencyChannelMaxEntries;
    for (uint32_t i = 0; i < block.entryCount && i < kLatencyChannelMaxEntries; ++i) {
        if (key == block.entries[i].key) {
            slot = i;
            break;
        }
    }
    if (slot == kLatencyChannelMaxEntries) {
        if (block.entryCount < kLatencyChannelMaxEntries) {
            slot = block.entryCount;
            ++block.entryCount;
        } else {
            // Full: drop the oldest entry and append. Endpoint churn within one session is rare,
            // and a lost entry only costs one re-probe.
            std::memmove(&block.entries[0], &block.entries[1],
                         sizeof(LatencyChannelEntry) * (kLatencyChannelMaxEntries - 1));
            slot = kLatencyChannelMaxEntries - 1;
        }
    }

    std::memset(block.entries[slot].key, 0, kLatencyChannelKeyCapacity);
    std::memcpy(block.entries[slot].key, key.c_str(), key.size());
    block.entries[slot].latencyMs = latencyMs;

    std::atomic_thread_fence(std::memory_order_release);
    block.sequence.fetch_add(1, std::memory_order_acq_rel);  // -> even: stable again
    return true;
}

// ---- Win32 mapping (av_sync_latency_channel.cpp) ----------------------------------------------

// Controller side: the process-lifetime session channel, created on first use. Returns the
// inheritable handle to hand to a disposable media child, or nullptr when the channel is
// unavailable (the child then simply probes, exactly as before).
void* GetSessionLatencyChannelChildHandle();

// Controller side: release the session channel. Called on controller shutdown; the mapping would
// also die with the process.
void ReleaseSessionLatencyChannel();

// Media side: map a channel handle inherited from the controller. Takes ownership of the handle
// and closes it either way (the view outlives it). Returns nullptr when the handle is absent or
// does not hold a compatible channel. The pointer stays valid until UnmapInheritedLatencyChannel.
LatencyChannelBlock* MapInheritedLatencyChannel(void* inheritedHandle);
void UnmapInheritedLatencyChannel(LatencyChannelBlock* block);

}  // namespace ce::av_sync
