// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include "hook/hooking/vtable_slot_reclaim.h"

TEST(VTableReclaimCollectionTest, BoundedCollectionSelectsOwnedSlotsInTableOrder) {
    void* table[6] = {};
    int original = 0;
    int detour = 0;
    ce::vtable_slot_reclaim::OwnedSlot output[2];
    const auto result = ce::vtable_slot_reclaim::GatherOwnedSlots(table, 6, output, [&](void** slot) {
        if (slot == table || slot == table + 3 || slot == table + 5)
            return ce::vtable_slot_reclaim::OwnedSlot{slot, &detour, &original};
        return ce::vtable_slot_reclaim::OwnedSlot{};
    });
    EXPECT_EQ(result.count, 2u);
    EXPECT_TRUE(result.truncated);
    EXPECT_EQ(output[0].slot, table);
    EXPECT_EQ(output[1].slot, table + 3);
    EXPECT_EQ(output[1].original, &original);
    EXPECT_EQ(output[1].detour, &detour);
}

TEST(VTableReclaimCollectionTest, EmptyAndExactCapacityCollectionsNeverWriteOutsideTheOutput) {
    void* table[2] = {};
    int original = 0;
    ce::vtable_slot_reclaim::OwnedSlot output[2];
    const auto lookup = [&](void** slot) { return ce::vtable_slot_reclaim::OwnedSlot{slot, &original, &original}; };
    const auto empty = ce::vtable_slot_reclaim::GatherOwnedSlots(table, 0, output, lookup);
    EXPECT_EQ(empty.count, 0u);
    EXPECT_FALSE(empty.truncated);
    const auto full = ce::vtable_slot_reclaim::GatherOwnedSlots(table, 2, output, lookup);
    EXPECT_EQ(full.count, 2u);
    EXPECT_FALSE(full.truncated);
    EXPECT_EQ(output[1].slot, table + 1);
    const auto noSpace = ce::vtable_slot_reclaim::GatherOwnedSlots(table, 2, {}, lookup);
    EXPECT_EQ(noSpace.count, 0u);
    EXPECT_TRUE(noSpace.truncated);
}
