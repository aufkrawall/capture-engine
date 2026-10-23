#pragma once

// Placement in the per-thread, reference-free shader memo of CE's D3D11 forced AF (d3d11_af_thread.h).
//
// Every shader may live in two buckets of kShaderBucketSlots aligned entries, chosen from its address by two
// independent hashes; a new shader takes a free entry in the less occupied bucket, and only when both are full does the
// least recently used of their entries go. Entries never move, so another thread can clear a destroyed shader from
// both buckets without a lock (ForgetShader), and a key the owner thread reads always names one live object.
//
// It used to be one window of 8 entries after a single hash: at a quarter of the capacity a few windows overflowed now
// and then, depending only on where the driver had allocated the shaders, and a 256-shader working set lost switches
// (FlowD3D11SamplerAFCost.RawShaderMemoHoldsNoShader, 2026-10-21). With two choices a bucket's occupancy stays near the
// average, so working sets far below capacity no longer evict.

#include <d3d11.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace ce::d3d11_af {

struct ShaderMemoEntry {
    // Set by the owner thread; cleared by any thread (ForgetShader).
    std::atomic<const ID3D11PixelShader*> shader{nullptr};
    IUnknown* planOwner = nullptr;  // owner thread: the shader's own metadata handle, borrowed
    uint32_t stamp = 0;
};

// BioShock Infinite (20261017_215604) switches among more shaders per frame than the 64 a context used to keep: half of
// its switches missed.
inline constexpr size_t kShaderEntries = 1024;
inline constexpr size_t kShaderBucketSlots = 8;
inline constexpr size_t kShaderBuckets = kShaderEntries / kShaderBucketSlots;
static_assert(kShaderBuckets == 128, "ShaderMemoBuckets takes 7 bits per hash");

struct ShaderMemoBucketPair {
    size_t first = 0;   // index of the first entry of each bucket
    size_t second = 0;  // never equal to `first`
};

// 32-bit arithmetic only: the x86 hook has no fast 64-bit multiply.
inline ShaderMemoBucketPair ShaderMemoBuckets(const ID3D11PixelShader* shader) {
    const auto address = reinterpret_cast<uintptr_t>(shader);
    uint32_t key = static_cast<uint32_t>(address >> 4);
    if constexpr (sizeof(uintptr_t) > sizeof(uint32_t)) {
        key ^= static_cast<uint32_t>(static_cast<uint64_t>(address) >> 32);
    }
    const uint32_t first = (key * 0x9E3779B1u) >> 25;
    uint32_t second = (key * 0x85EBCA77u) >> 25;
    if (second == first) {
        second = (first + 1) & (kShaderBuckets - 1);
    }
    return {first * kShaderBucketSlots, second * kShaderBucketSlots};
}

// The entry naming `shader`, or null. Owner thread.
inline ShaderMemoEntry* FindShaderMemoEntry(ShaderMemoEntry* entries, const ID3D11PixelShader* shader) {
    const ShaderMemoBucketPair buckets = ShaderMemoBuckets(shader);
    for (const size_t start : {buckets.first, buckets.second}) {
        for (size_t slot = start; slot < start + kShaderBucketSlots; ++slot) {
            if (entries[slot].shader.load(std::memory_order_acquire) == shader) {
                return &entries[slot];
            }
        }
    }
    return nullptr;
}

// Remembers `shader` with `planOwner`; refreshes an existing entry. True when a used entry had to go. Owner thread.
inline bool PlaceShaderMemoEntry(ShaderMemoEntry* entries, uint32_t& clock, const ID3D11PixelShader* shader,
                                 IUnknown* planOwner) {
    if (ShaderMemoEntry* held = FindShaderMemoEntry(entries, shader)) {
        held->planOwner = planOwner;
        held->stamp = ++clock;
        return false;
    }
    const ShaderMemoBucketPair buckets = ShaderMemoBuckets(shader);
    ShaderMemoEntry* freeEntry[2] = {nullptr, nullptr};
    size_t used[2] = {0, 0};
    ShaderMemoEntry* oldest = nullptr;
    const size_t starts[2] = {buckets.first, buckets.second};
    for (size_t bucket = 0; bucket < 2; ++bucket) {
        for (size_t slot = starts[bucket]; slot < starts[bucket] + kShaderBucketSlots; ++slot) {
            ShaderMemoEntry& entry = entries[slot];
            if (!entry.shader.load(std::memory_order_acquire)) {
                if (!freeEntry[bucket]) {
                    freeEntry[bucket] = &entry;
                }
                continue;
            }
            ++used[bucket];
            if (!oldest || entry.stamp < oldest->stamp) {
                oldest = &entry;
            }
        }
    }
    // The less occupied bucket with room; both full: the least recently used entry of the two.
    ShaderMemoEntry* target = nullptr;
    if (freeEntry[0] && (!freeEntry[1] || used[0] <= used[1])) {
        target = freeEntry[0];
    } else if (freeEntry[1]) {
        target = freeEntry[1];
    }
    const bool evicted = target == nullptr;
    if (evicted) {
        target = oldest;
    }
    if (!target) {
        return false;
    }
    // The key goes last: the entry names the shader only once its handle is in place.
    target->shader.store(nullptr, std::memory_order_seq_cst);
    target->planOwner = planOwner;
    target->stamp = ++clock;
    target->shader.store(shader, std::memory_order_seq_cst);
    return evicted;
}

// Clears `shader` from both of its buckets. Any thread; lock-free.
inline void ForgetShaderMemoEntry(ShaderMemoEntry* entries, const ID3D11PixelShader* shader) {
    const ShaderMemoBucketPair buckets = ShaderMemoBuckets(shader);
    for (const size_t start : {buckets.first, buckets.second}) {
        for (size_t slot = start; slot < start + kShaderBucketSlots; ++slot) {
            ShaderMemoEntry& entry = entries[slot];
            const ID3D11PixelShader* expected = shader;
            if (entry.shader.load(std::memory_order_seq_cst) == shader) {
                entry.shader.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst);
            }
        }
    }
}

}  // namespace ce::d3d11_af
