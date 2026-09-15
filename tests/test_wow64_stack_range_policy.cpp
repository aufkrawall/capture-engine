#include <gtest/gtest.h>

#include <vector>

#include "../common/wow64_stack_range_policy.h"

// A 64-bit MiniDumpWriteDump against a WoW64 target records the x64 side of
// every thread and nothing of the 32-bit side, so the dump cannot produce a
// single caller. These tests pin the range arithmetic that decides which extra
// memory the dump has to be told to include.

namespace {

using ce::wow64_stack_ranges::Range;
using ce::wow64_stack_ranges::RangeBudget;
using ce::wow64_stack_ranges::Region;

// One thread stack as Windows lays it out: a single reservation split into an
// uncommitted tail, a guard page, and the committed part in use.
class FakeAddressSpace {
public:
    void AddRegion(uint64_t base, uint64_t size, uint64_t allocationBase, bool committed) {
        regions_.push_back(Region{base, size, allocationBase, committed, true});
    }

    Region operator()(uint64_t address) const {
        for (const Region& region : regions_) {
            if (address >= region.base && address < region.base + region.size) {
                return region;
            }
        }
        return Region{};
    }

private:
    std::vector<Region> regions_;
};

constexpr uint64_t kReservation = 0x00b60000;
constexpr uint64_t kStackTop = 0x01360000;

}  // namespace

TEST(Wow64StackRangePolicyTest, ResolvesTheCommittedStackAboveTheStackPointer) {
    FakeAddressSpace space;
    space.AddRegion(kReservation, 0x1345000 - kReservation, kReservation, false);
    space.AddRegion(0x1345000, kStackTop - 0x1345000, kReservation, true);

    Range range;
    // The observed crash frame from Gothic II session 20260916_000027.
    ASSERT_TRUE(ce::wow64_stack_ranges::ResolveStackRange(0x0135f468, space, range));
    EXPECT_EQ(range.start, 0x0135f468u);
    EXPECT_EQ(range.size, kStackTop - 0x0135f468u);
}

TEST(Wow64StackRangePolicyTest, WalksAcrossAdjacentRegionsOfTheSameReservation) {
    FakeAddressSpace space;
    space.AddRegion(0x1340000, 0x10000, kReservation, true);
    space.AddRegion(0x1350000, 0x10000, kReservation, true);
    // A different reservation directly above must not be swept in.
    space.AddRegion(0x1360000, 0x10000, 0x1360000, true);

    Range range;
    ASSERT_TRUE(ce::wow64_stack_ranges::ResolveStackRange(0x1345000, space, range));
    EXPECT_EQ(range.start, 0x1345000u);
    EXPECT_EQ(range.size, 0x1360000u - 0x1345000u);
}

TEST(Wow64StackRangePolicyTest, StopsAtTheFirstUncommittedRegionAbove) {
    FakeAddressSpace space;
    space.AddRegion(0x1340000, 0x10000, kReservation, true);
    space.AddRegion(0x1350000, 0x10000, kReservation, false);

    Range range;
    ASSERT_TRUE(ce::wow64_stack_ranges::ResolveStackRange(0x1348000, space, range));
    EXPECT_EQ(range.size, 0x1350000u - 0x1348000u);
}

TEST(Wow64StackRangePolicyTest, RejectsAStackPointerOutsideCommittedMemory) {
    FakeAddressSpace space;
    space.AddRegion(0x1340000, 0x10000, kReservation, false);

    Range range;
    EXPECT_FALSE(ce::wow64_stack_ranges::ResolveStackRange(0x1345000, space, range));
    EXPECT_FALSE(ce::wow64_stack_ranges::ResolveStackRange(0x9000000, space, range));
    EXPECT_FALSE(ce::wow64_stack_ranges::ResolveStackRange(0, space, range));
}

TEST(Wow64StackRangePolicyTest, RejectsAStackPointerOutsideTheThirtyTwoBitAddressSpace) {
    FakeAddressSpace space;
    space.AddRegion(0x1340000, 0x10000, kReservation, true);

    Range range;
    // A 64-bit RSP must never be mistaken for a WoW64 stack pointer.
    EXPECT_FALSE(ce::wow64_stack_ranges::ResolveStackRange(0x000000000aed18ull + (1ull << 32), space, range));
}

TEST(Wow64StackRangePolicyTest, ClampsAnImplausiblyLargeStackToThePerThreadCap) {
    FakeAddressSpace space;
    space.AddRegion(0x1000000, 0x20000000, 0x1000000, true);

    Range range;
    ASSERT_TRUE(ce::wow64_stack_ranges::ResolveStackRange(0x1000000, space, range));
    EXPECT_EQ(range.size, ce::wow64_stack_ranges::kMaxRangeBytesPerThread);
}

TEST(Wow64StackRangePolicyTest, BudgetTruncatesTheLastRangeInsteadOfDroppingIt) {
    RangeBudget budget;
    Range range{0x1000, ce::wow64_stack_ranges::kMaxTotalRangeBytes - 16};
    ASSERT_TRUE(budget.Admit(range));

    Range tail{0x2000, 4096};
    ASSERT_TRUE(budget.Admit(tail));
    // The bytes nearest the stack pointer carry the call chain, so what fits is
    // kept rather than the whole range being refused.
    EXPECT_EQ(tail.size, 16u);
    EXPECT_EQ(budget.UsedBytes(), ce::wow64_stack_ranges::kMaxTotalRangeBytes);

    Range overflow{0x3000, 4096};
    EXPECT_FALSE(budget.Admit(overflow));
    EXPECT_EQ(budget.Count(), 2u);
}

TEST(Wow64StackRangePolicyTest, BudgetRefusesEmptyRangesAndBoundsTheCount) {
    RangeBudget budget;
    Range empty{0x1000, 0};
    EXPECT_FALSE(budget.Admit(empty));

    for (size_t i = 0; i < ce::wow64_stack_ranges::kMaxRanges; ++i) {
        Range range{0x1000 + i * 0x1000, 8};
        ASSERT_TRUE(budget.Admit(range)) << "range " << i;
    }
    Range extra{0xF000000, 8};
    EXPECT_FALSE(budget.Admit(extra));
    EXPECT_EQ(budget.Count(), ce::wow64_stack_ranges::kMaxRanges);
}
