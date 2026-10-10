#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "common/crash/fault_neighborhood_policy.h"

namespace policy = ce::fault_neighborhood;

namespace {

// A code sample with one rip-relative LEA, one relative CALL and one
// rip-relative MOV, interleaved with padding. Expected targets from base B:
// lea at 0 -> B + 7 + 0x10, call at 8 -> B + 13 + 5, mov at 16 -> B + 23 + 0x20.
std::vector<uint8_t> SampleCode() {
    std::vector<uint8_t> code(32, 0x90);
    const uint8_t lea[] = {0x48, 0x8D, 0x0D, 0x10, 0x00, 0x00, 0x00};
    std::memcpy(code.data() + 0, lea, sizeof(lea));
    const uint8_t call[] = {0xE8, 0x05, 0x00, 0x00, 0x00};
    std::memcpy(code.data() + 8, call, sizeof(call));
    const uint8_t mov[] = {0x4C, 0x8B, 0x05, 0x20, 0x00, 0x00, 0x00};
    std::memcpy(code.data() + 16, mov, sizeof(mov));
    return code;
}

policy::Region CommittedRegion(uint64_t base, uint64_t size, uint64_t allocationBase = 0) {
    policy::Region region;
    region.base = base;
    region.size = size;
    region.allocationBase = allocationBase != 0 ? allocationBase : base;
    region.committed = true;
    region.readable = true;
    region.valid = true;
    return region;
}

}  // namespace

TEST(FaultNeighborhoodPolicyTest, CodeWindowStraddlesThePointerWithMoreRoomAfter) {
    const policy::Range window = policy::CodeWindow(0x10000);
    EXPECT_EQ(window.start, 0x10000 - policy::kCodeWindowBeforeBytes);
    EXPECT_EQ(window.size, policy::kCodeWindowBeforeBytes + policy::kCodeWindowAfterBytes);
}

TEST(FaultNeighborhoodPolicyTest, CodeWindowClampsAtAddressZero) {
    const policy::Range window = policy::CodeWindow(0x10);
    EXPECT_EQ(window.start, 0u);
    EXPECT_EQ(window.size, policy::kCodeWindowBeforeBytes + policy::kCodeWindowAfterBytes);
}

TEST(FaultNeighborhoodPolicyTest, ReferenceWindowAlignsDownAndCoversThePointer) {
    const policy::Range window = policy::ReferenceWindow(0x4321, 512);
    EXPECT_EQ(window.start, 0x4320u);
    EXPECT_GE(window.start + window.size, 0x4321u + 512);
}

TEST(FaultNeighborhoodPolicyTest, RangesOverlapIsFalseForAdjacentRanges) {
    const policy::Range a{0x1000, 0x100};
    const policy::Range b{0x1100, 0x100};
    const policy::Range c{0x1080, 0x100};
    EXPECT_FALSE(policy::RangesOverlap(a, b));
    EXPECT_FALSE(policy::RangesOverlap(b, a));
    EXPECT_TRUE(policy::RangesOverlap(a, c));
}

TEST(FaultNeighborhoodPolicyTest, BudgetTruncatesInsteadOfDropping) {
    policy::Budget budget;
    policy::Range first{0x1000, policy::kMaxTotalBytes / 2};
    policy::Range second{0x20000000, policy::kMaxTotalBytes};
    ASSERT_TRUE(budget.Admit(first));
    ASSERT_TRUE(budget.Admit(second));
    EXPECT_EQ(second.size, policy::kMaxTotalBytes - first.size);
    EXPECT_EQ(budget.UsedBytes(), policy::kMaxTotalBytes);
    policy::Range third{0x30000000, 1};
    EXPECT_FALSE(budget.Admit(third));
}

TEST(FaultNeighborhoodPolicyTest, BudgetCountsRanges) {
    policy::Budget budget;
    for (size_t i = 0; i < policy::kMaxRanges; ++i) {
        policy::Range range{0x1000 * (i + 1), 1};
        ASSERT_TRUE(budget.Admit(range));
    }
    policy::Range extra{0x7FFFFFFF000, 1};
    EXPECT_FALSE(budget.Admit(extra));
}

TEST(FaultNeighborhoodPolicyTest, HarvestFindsCallsAndRipRelativeDataReferences) {
    const std::vector<uint8_t> code = SampleCode();
    std::vector<policy::CodeReference> refs;
    policy::HarvestCodeReferences(code.data(), code.size(), 0x7FF70000, 16, refs);
    ASSERT_EQ(refs.size(), 3u);
    EXPECT_EQ(refs[0].address, 0x7FF70000u + 0x17);  // lea at 0
    EXPECT_FALSE(refs[0].code);
    EXPECT_EQ(refs[1].address, 0x7FF70000u + 0x12);  // call at 8
    EXPECT_TRUE(refs[1].code);
    EXPECT_EQ(refs[2].address, 0x7FF70000u + 0x37);  // mov at 16
    EXPECT_FALSE(refs[2].code);
}

TEST(FaultNeighborhoodPolicyTest, HarvestDeduplicatesTargets) {
    std::vector<uint8_t> code(16, 0x90);
    const uint8_t lea[] = {0x48, 0x8D, 0x0D, 0x10, 0x00, 0x00, 0x00};
    std::memcpy(code.data() + 0, lea, sizeof(lea));
    std::memcpy(code.data() + 8, lea, sizeof(lea));
    std::vector<policy::CodeReference> refs;
    policy::HarvestCodeReferences(code.data(), code.size(), 0x1000, 16, refs);
    // The second LEA computes a different rip base and lands elsewhere; what
    // must never happen is the same target twice.
    for (size_t i = 0; i < refs.size(); ++i) {
        for (size_t j = i + 1; j < refs.size(); ++j) {
            EXPECT_NE(refs[i].address, refs[j].address);
        }
    }
}

TEST(FaultNeighborhoodPolicyTest, HarvestRespectsTheReferenceCap) {
    const std::vector<uint8_t> code = SampleCode();
    std::vector<policy::CodeReference> refs;
    policy::HarvestCodeReferences(code.data(), code.size(), 0x1000, 1, refs);
    EXPECT_EQ(refs.size(), 1u);
}

TEST(FaultNeighborhoodPolicyTest, HarvestIgnoresTruncatedCode) {
    const std::vector<uint8_t> code = SampleCode();
    std::vector<policy::CodeReference> refs;
    policy::HarvestCodeReferences(code.data(), 3, 0x1000, 16, refs);
    EXPECT_TRUE(refs.empty());
}

TEST(FaultNeighborhoodPolicyTest, CommittedSpanFollowsTheStackReservation) {
    const auto query = [](uint64_t address) {
        if (address >= 0x1000 && address < 0x2000) return CommittedRegion(0x1000, 0x1000, 0x1000);
        if (address >= 0x2000 && address < 0x3000) return CommittedRegion(0x2000, 0x1000, 0x1000);
        return policy::Region{};
    };
    policy::Range span;
    ASSERT_TRUE(policy::ResolveCommittedSpanAbove(0x1800, 1ull << 20, query, span));
    EXPECT_EQ(span.start, 0x1800u);
    EXPECT_EQ(span.size, 0x1800u);  // 0x1800..0x3000, stopping at the foreign region
}

TEST(FaultNeighborhoodPolicyTest, CommittedSpanIsCapped) {
    const auto query = [](uint64_t address) {
        return CommittedRegion(address & ~uint64_t{0xFFF}, 0x1000, 0x1000);
    };
    policy::Range span;
    ASSERT_TRUE(policy::ResolveCommittedSpanAbove(0x1000, 0x1800, query, span));
    EXPECT_EQ(span.size, 0x1800u);
}

TEST(FaultNeighborhoodPolicyTest, CommittedSpanRejectsUncommittedAndForeignRegions) {
    policy::Range span;
    EXPECT_FALSE(policy::ResolveCommittedSpanAbove(0x1000, 0x1000, [](uint64_t) { return policy::Region{}; }, span));

    const auto mixed = [](uint64_t address) {
        if (address == 0x1000) return CommittedRegion(0x1000, 0x1000, 0x1000);
        return CommittedRegion(0x2000, 0x1000, 0x9000);
    };
    ASSERT_TRUE(policy::ResolveCommittedSpanAbove(0x1000, 1ull << 20, mixed, span));
    EXPECT_EQ(span.size, 0x1000u);
}

namespace {

std::vector<uint8_t> SamplePeHeaders() {
    // DOS header with e_lfanew=0x80, a PE header with two sections and a
    // 0xF0-byte optional header: .text (code) and .data (write).
    std::vector<uint8_t> headers(0x200, 0);
    headers[0] = 'M';
    headers[1] = 'Z';
    const uint32_t lfanew = 0x80;
    std::memcpy(headers.data() + 0x3C, &lfanew, sizeof(lfanew));
    uint8_t* nt = headers.data() + 0x80;
    nt[0] = 'P';
    nt[1] = 'E';
    const uint16_t sectionCount = 2;
    const uint16_t optionalHeaderSize = 0xF0;
    std::memcpy(nt + 4 + 2, &sectionCount, sizeof(sectionCount));
    std::memcpy(nt + 4 + 16, &optionalHeaderSize, sizeof(optionalHeaderSize));
    uint8_t* text = headers.data() + 0x80 + 4 + 20 + 0xF0;
    uint32_t virtualSize = 0x1000;
    uint32_t virtualAddress = 0x1000;
    uint32_t rawSize = 0x800;
    uint32_t characteristics = 0x60000020;
    std::memcpy(text + 8, &virtualSize, 4);
    std::memcpy(text + 12, &virtualAddress, 4);
    std::memcpy(text + 16, &rawSize, 4);
    std::memcpy(text + 36, &characteristics, 4);
    uint8_t* data = text + 40;
    virtualSize = 0x2000;
    virtualAddress = 0x5000;
    rawSize = 0x1000;
    characteristics = 0xC0000040;
    std::memcpy(data + 8, &virtualSize, 4);
    std::memcpy(data + 12, &virtualAddress, 4);
    std::memcpy(data + 16, &rawSize, 4);
    std::memcpy(data + 36, &characteristics, 4);
    return headers;
}

}  // namespace

TEST(FaultNeighborhoodPolicyTest, WritableDataSectionsKeepDataAndDropCode) {
    const std::vector<uint8_t> headers = SamplePeHeaders();
    std::vector<policy::SectionRange> sections;
    ASSERT_TRUE(policy::HarvestWritableDataSections(headers.data(), headers.size(), sections));
    ASSERT_EQ(sections.size(), 1u);
    EXPECT_EQ(sections[0].rva, 0x5000u);
    EXPECT_EQ(sections[0].size, 0x2000u);  // virtual size wins over raw size
}

TEST(FaultNeighborhoodPolicyTest, WritableDataSectionsRejectBrokenHeaders) {
    std::vector<policy::SectionRange> sections;
    EXPECT_FALSE(policy::HarvestWritableDataSections(nullptr, 100, sections));

    std::vector<uint8_t> headers = SamplePeHeaders();
    headers.resize(0x50);
    EXPECT_FALSE(policy::HarvestWritableDataSections(headers.data(), headers.size(), sections));

    headers = SamplePeHeaders();
    headers[0x80] = 'X';
    EXPECT_FALSE(policy::HarvestWritableDataSections(headers.data(), headers.size(), sections));
}
