// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// Placement in the forced-AF shader memo (hook/d3d11/d3d11_af_shader_memo.h): two buckets per shader, the less
// occupied one first, so a working set far below capacity never loses an entry to where its addresses happen to hash.
// The single 8-entry window it replaced overflowed now and then at a quarter of the capacity
// (FlowD3D11SamplerAFCost.RawShaderMemoHoldsNoShader, 2026-10-21).
#include "hook/d3d11/d3d11_af_shader_memo.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

using ce::d3d11_af::FindShaderMemoEntry;
using ce::d3d11_af::ForgetShaderMemoEntry;
using ce::d3d11_af::kShaderBucketSlots;
using ce::d3d11_af::kShaderEntries;
using ce::d3d11_af::PlaceShaderMemoEntry;
using ce::d3d11_af::ShaderMemoBuckets;
using ce::d3d11_af::ShaderMemoEntry;

// Shaders are only compared by address here, never dereferenced.
const ID3D11PixelShader* Shader(uintptr_t address) {
    return reinterpret_cast<const ID3D11PixelShader*>(address);
}

IUnknown* Plan(uintptr_t tag) {
    return reinterpret_cast<IUnknown*>(tag);
}

struct Memo {
    std::unique_ptr<ShaderMemoEntry[]> entries{new ShaderMemoEntry[kShaderEntries]};
    uint32_t clock = 0;
    bool Place(const ID3D11PixelShader* shader) {
        return PlaceShaderMemoEntry(entries.get(), clock, shader, Plan(reinterpret_cast<uintptr_t>(shader) | 1));
    }
    bool Holds(const ID3D11PixelShader* shader) { return FindShaderMemoEntry(entries.get(), shader) != nullptr; }
};

// Addresses from `start` on, 16-byte aligned like every COM object, that satisfy `match`.
template <typename Match>
std::vector<uintptr_t> Addresses(size_t count, uintptr_t start, Match match) {
    std::vector<uintptr_t> found;
    for (uintptr_t address = start; found.size() < count; address += 16) {
        if (match(address))
            found.push_back(address);
    }
    return found;
}

}  // namespace

TEST(D3D11AFShaderMemoTest, EveryShaderHasTwoDistinctAlignedBuckets) {
    for (uintptr_t address = 0x10000; address < 0x10000 + 16 * 20000; address += 16) {
        const auto buckets = ShaderMemoBuckets(Shader(address));
        ASSERT_NE(buckets.first, buckets.second);
        ASSERT_EQ(buckets.first % kShaderBucketSlots, 0u);
        ASSERT_EQ(buckets.second % kShaderBucketSlots, 0u);
        ASSERT_LT(buckets.first, kShaderEntries);
        ASSERT_LT(buckets.second, kShaderEntries);
    }
}

// Twice a bucket's worth of shaders that share their first bucket: the old window kept 8 of them.
TEST(D3D11AFShaderMemoTest, ShadersCrowdingOneBucketSpillIntoTheirSecond) {
    const size_t crowded = ShaderMemoBuckets(Shader(0x7FF600010000)).first;
    const std::vector<uintptr_t> addresses = Addresses(2 * kShaderBucketSlots, 0x7FF600010000, [&](uintptr_t address) {
        return ShaderMemoBuckets(Shader(address)).first == crowded;
    });
    Memo memo;
    for (const uintptr_t address : addresses)
        EXPECT_FALSE(memo.Place(Shader(address))) << std::hex << address;
    for (const uintptr_t address : addresses)
        EXPECT_TRUE(memo.Holds(Shader(address))) << std::hex << address;
}

// A quarter and a half of the capacity, laid out as a driver allocates shaders (a fixed stride) and scattered.
TEST(D3D11AFShaderMemoTest, WorkingSetsFarBelowCapacityPlaceWithoutEviction) {
    for (const size_t count : {kShaderEntries / 4, kShaderEntries / 2}) {
        for (const uintptr_t stride : {uintptr_t{0x1A0}, uintptr_t{0x240}, uintptr_t{0x1000}}) {
            Memo memo;
            for (size_t index = 0; index < count; ++index)
                ASSERT_FALSE(memo.Place(Shader(0x1F4C0000000 + index * stride))) << count << " stride " << stride;
            for (size_t index = 0; index < count; ++index)
                ASSERT_TRUE(memo.Holds(Shader(0x1F4C0000000 + index * stride)));
        }
        Memo scattered;
        uint32_t state = 0x2545F491u;
        std::vector<uintptr_t> addresses;
        for (size_t index = 0; index < count; ++index) {
            state = state * 1664525u + 1013904223u;
            addresses.push_back(0x7FF000000000 + (static_cast<uintptr_t>(state) << 4));
        }
        for (const uintptr_t address : addresses)
            ASSERT_FALSE(scattered.Place(Shader(address))) << count << " scattered " << std::hex << address;
        for (const uintptr_t address : addresses)
            ASSERT_TRUE(scattered.Holds(Shader(address)));
    }
}

// 256 scattered shaders on which the old single window (8 entries after one hash, LRU) lost an entry: one window had
// more homes than room. Simulated against the old placement, about 1 in 100 random 256-shader sets did; against this
// one none of 20000 did, nor of 5000 at 512 shaders (evictions start near three quarters of the capacity).
TEST(D3D11AFShaderMemoTest, TheSetTheOldWindowCouldNotHoldFits) {
    uint32_t state = 27;
    std::vector<uintptr_t> addresses;
    for (size_t index = 0; index < kShaderEntries / 4; ++index) {
        state = state * 1664525u + 1013904223u;
        addresses.push_back(0x7FF000000000 + (static_cast<uintptr_t>(state) << 4));
    }
    Memo memo;
    for (const uintptr_t address : addresses)
        EXPECT_FALSE(memo.Place(Shader(address))) << std::hex << address;
    for (const uintptr_t address : addresses)
        EXPECT_TRUE(memo.Holds(Shader(address))) << std::hex << address;
}

TEST(D3D11AFShaderMemoTest, AForgottenShaderLeavesWhicheverBucketHeldIt) {
    const size_t crowded = ShaderMemoBuckets(Shader(0x40000)).first;
    const std::vector<uintptr_t> addresses = Addresses(kShaderBucketSlots + 2, 0x40000, [&](uintptr_t address) {
        return ShaderMemoBuckets(Shader(address)).first == crowded;
    });
    Memo memo;
    for (const uintptr_t address : addresses)
        ASSERT_FALSE(memo.Place(Shader(address)));
    for (const uintptr_t address : addresses) {
        ForgetShaderMemoEntry(memo.entries.get(), Shader(address));
        EXPECT_FALSE(memo.Holds(Shader(address))) << std::hex << address;
    }
    ForgetShaderMemoEntry(memo.entries.get(), Shader(0x123450));  // never held: nothing to clear
    EXPECT_FALSE(memo.Place(Shader(addresses.front()))) << "its entries are free again";
    EXPECT_TRUE(memo.Holds(Shader(addresses.front())));
}

// Only when both buckets are full does an entry go: the least recently used of the two.
TEST(D3D11AFShaderMemoTest, WithBothBucketsFullTheLeastRecentlyUsedGoes) {
    const auto pair = ShaderMemoBuckets(Shader(0x900000));
    const std::vector<uintptr_t> addresses = Addresses(2 * kShaderBucketSlots + 1, 0x900000, [&](uintptr_t address) {
        const auto buckets = ShaderMemoBuckets(Shader(address));
        return buckets.first == pair.first && buckets.second == pair.second;
    });
    Memo memo;
    for (size_t index = 0; index < 2 * kShaderBucketSlots; ++index)
        ASSERT_FALSE(memo.Place(Shader(addresses[index]))) << index;
    // Everyone but the fourth is used again, so it is the oldest.
    for (size_t index = 0; index < 2 * kShaderBucketSlots; ++index) {
        if (index != 3)
            ASSERT_FALSE(memo.Place(Shader(addresses[index])));
    }
    EXPECT_TRUE(memo.Place(Shader(addresses.back()))) << "both buckets were full";
    EXPECT_FALSE(memo.Holds(Shader(addresses[3])));
    EXPECT_TRUE(memo.Holds(Shader(addresses.back())));
    for (size_t index = 0; index < 2 * kShaderBucketSlots; ++index) {
        if (index != 3)
            EXPECT_TRUE(memo.Holds(Shader(addresses[index]))) << index;
    }
}

TEST(D3D11AFShaderMemoTest, ARememberedShaderRefreshesItsPlan) {
    Memo memo;
    ASSERT_FALSE(PlaceShaderMemoEntry(memo.entries.get(), memo.clock, Shader(0x5550), Plan(0x10)));
    ASSERT_FALSE(PlaceShaderMemoEntry(memo.entries.get(), memo.clock, Shader(0x5550), Plan(0x20)));
    const ShaderMemoEntry* entry = FindShaderMemoEntry(memo.entries.get(), Shader(0x5550));
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->planOwner, Plan(0x20));
}
