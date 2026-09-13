#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../hook/wrappers/d3dkmt_abi.h"

namespace {

using ce::d3dkmt::AdapterInfo;
using ce::d3dkmt::EnumAdapters;
using ce::d3dkmt::EnumAdapters2;
using ce::d3dkmt::Handle;
using ce::d3dkmt::QueryAdapterInfo;
using ce::d3dkmt::QueryVideoMemoryInfo;

// The mirrored structures belong to the application and the display kernel, not
// to CE: every field CE reads or writes is addressed by offset. The header's
// static_asserts already fail the build on a layout change; these cases make the
// same contract readable, and they are the ones that would have caught the
// UINT64 `D3DKMT_HANDLE` this file was written to regress
// (`hAdapter` is UINT32 in d3dukmdt.h, and typing it as a pointer-sized handle
// shifted every later member by eight bytes).
TEST(D3DKMTAbi, HandleIsThirtyTwoBitsWideLikeTheKernelThunkDeclaresIt) {
    EXPECT_EQ(sizeof(Handle), 4u);
    EXPECT_EQ(offsetof(QueryAdapterInfo, Type), 4u);
    EXPECT_EQ(offsetof(QueryVideoMemoryInfo, MemorySegmentGroup), sizeof(HANDLE) + 4u);
}

TEST(D3DKMTAbi, QueryAdapterInfoMatchesD3dkmthk) {
    EXPECT_EQ(offsetof(QueryAdapterInfo, hAdapter), 0u);
    EXPECT_EQ(offsetof(QueryAdapterInfo, Type), 4u);
    EXPECT_EQ(offsetof(QueryAdapterInfo, pPrivateDriverData), 8u);
    EXPECT_EQ(offsetof(QueryAdapterInfo, PrivateDriverDataSize), 8u + sizeof(void*));
    EXPECT_EQ(sizeof(QueryAdapterInfo), sizeof(void*) == 8 ? 24u : 16u);
}

TEST(D3DKMTAbi, QueryVideoMemoryInfoMatchesD3dkmthk) {
    EXPECT_EQ(offsetof(QueryVideoMemoryInfo, hProcess), 0u);
    EXPECT_EQ(offsetof(QueryVideoMemoryInfo, hAdapter), sizeof(HANDLE));
    EXPECT_EQ(offsetof(QueryVideoMemoryInfo, MemorySegmentGroup), sizeof(HANDLE) + 4u);
    EXPECT_EQ(offsetof(QueryVideoMemoryInfo, Budget), 16u);
    EXPECT_EQ(offsetof(QueryVideoMemoryInfo, CurrentUsage), 24u);
    EXPECT_EQ(offsetof(QueryVideoMemoryInfo, CurrentReservation), 32u);
    EXPECT_EQ(offsetof(QueryVideoMemoryInfo, AvailableForReservation), 40u);
    EXPECT_EQ(offsetof(QueryVideoMemoryInfo, PhysicalAdapterIndex), 48u);
    EXPECT_EQ(sizeof(QueryVideoMemoryInfo), 56u);
}

TEST(D3DKMTAbi, AdapterInfoMatchesD3dkmthk) {
    // LUID aligns to 4, so it packs directly behind the 4-byte handle.
    EXPECT_EQ(offsetof(AdapterInfo, hAdapter), 0u);
    EXPECT_EQ(offsetof(AdapterInfo, AdapterLuid), 4u);
    EXPECT_EQ(offsetof(AdapterInfo, NumOfSources), 12u);
    EXPECT_EQ(offsetof(AdapterInfo, bPrecisePresentRegionsPreferred), 16u);
    EXPECT_EQ(sizeof(AdapterInfo), 20u);
    EXPECT_EQ(offsetof(EnumAdapters, Adapters), 4u);
    EXPECT_EQ(offsetof(EnumAdapters2, pAdapters), sizeof(void*));
}

// The concrete misread the wrong mirror produced: with a 64-bit handle, reading
// `Type` landed on the low dword of `pPrivateDriverData` and
// `PrivateDriverDataSize` landed past the structure. DOOM Eternal session
// 20260913_180809 logged `Type=3086977864, Size=52413` from exactly that.
TEST(D3DKMTAbi, QueryAdapterInfoFieldsReadWhatTheCallerWrote) {
    alignas(8) unsigned char storage[sizeof(QueryAdapterInfo) + 8] = {};
    auto* info = reinterpret_cast<QueryAdapterInfo*>(storage);
    uint32_t privateData = 0;

    info->hAdapter = 0x1234u;
    info->Type = ce::d3dkmt::kQaiTypeAdapterRegistryInfo;
    info->pPrivateDriverData = &privateData;
    info->PrivateDriverDataSize = sizeof(privateData);

    EXPECT_EQ(info->hAdapter, 0x1234u);
    EXPECT_EQ(static_cast<unsigned>(info->Type), 8u);
    EXPECT_EQ(info->pPrivateDriverData, &privateData);
    EXPECT_EQ(info->PrivateDriverDataSize, sizeof(privateData));

    // The 32-bit read at offset 4 is the Type the caller set, not a pointer half.
    uint32_t typeWord = 0;
    std::memcpy(&typeWord, storage + 4, sizeof(typeWord));
    EXPECT_EQ(typeWord, 8u);
}

// The override branch writes Budget/CurrentUsage/CurrentReservation/
// AvailableForReservation back into the caller's structure. A shifted mirror
// would have put the budget where CurrentUsage lives.
TEST(D3DKMTAbi, VideoMemoryOverrideFieldsLandOnTheirOwnSlots) {
    QueryVideoMemoryInfo info = {};
    info.MemorySegmentGroup = ce::d3dkmt::kSegmentGroupLocal;
    info.Budget = 0x1111'1111'1111'1111ull;
    info.CurrentUsage = 0x2222'2222'2222'2222ull;
    info.CurrentReservation = 0x3333'3333'3333'3333ull;
    info.AvailableForReservation = 0x4444'4444'4444'4444ull;

    uint64_t word = 0;
    std::memcpy(&word, reinterpret_cast<const unsigned char*>(&info) + 16, sizeof(word));
    EXPECT_EQ(word, 0x1111'1111'1111'1111ull);
    std::memcpy(&word, reinterpret_cast<const unsigned char*>(&info) + 24, sizeof(word));
    EXPECT_EQ(word, 0x2222'2222'2222'2222ull);

    uint32_t group = 0;
    std::memcpy(&group, reinterpret_cast<const unsigned char*>(&info) + sizeof(HANDLE) + 4, sizeof(group));
    EXPECT_EQ(group, static_cast<uint32_t>(ce::d3dkmt::kSegmentGroupLocal));
}

}  // namespace
