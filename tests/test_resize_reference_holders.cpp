#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "hook/present/resize_reference_holders.h"
#include "source_fragment_reader.h"

namespace holders = ce::resize_reference_holders;

namespace {

// A fake address space: two "images" and a block of memory at kBlockBase.
constexpr uintptr_t kImageA = 0x10000000;
constexpr uintptr_t kImageB = 0x20000000;
constexpr uintptr_t kBlockBase = 0x7000000000;

std::vector<holders::ImageRange> Images() {
    return {{kImageA, kImageA + 0x100000, 0}, {kImageB, kImageB + 0x80000, 1}};
}

void Put(std::vector<uint8_t>& block, size_t offset, uintptr_t value) {
    memcpy(block.data() + offset, &value, sizeof(value));
}

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

}  // namespace

TEST(ResizeReferenceHoldersTest, FindsEverySlotAndNamesTheObjectByItsVtable) {
    const auto images = Images();
    const uintptr_t buffers[] = {0x5550000, 0x5551000, 0x5552000};
    std::vector<uint8_t> block(4096, 0);
    // An object of image B: vtable at +0x100, its three back-buffer members after it.
    Put(block, 0x100, kImageB + 0x4F00);
    Put(block, 0x118, buffers[0]);
    Put(block, 0x120, buffers[1]);
    Put(block, 0x128, buffers[2]);
    // A second holder of image A, 0x40 bytes past its vtable.
    Put(block, 0x800, kImageA + 0x1234);
    Put(block, 0x840, buffers[1]);

    holders::Hit hits[8];
    const size_t found = holders::ScanBlock(block.data(), block.size(), kBlockBase, kBlockBase, buffers, 3,
                                            images.data(), images.size(), hits, 8);
    ASSERT_EQ(found, 4u);
    EXPECT_EQ(hits[0].address, kBlockBase + 0x118);
    EXPECT_EQ(hits[0].target, 0u);
    ASSERT_TRUE(hits[0].ownerFound);
    EXPECT_EQ(hits[0].ownerImage, 1u);
    EXPECT_EQ(hits[0].ownerRva, 0x4F00u);
    EXPECT_EQ(hits[0].ownerDistance, 0x18u);
    EXPECT_EQ(hits[2].target, 2u);
    EXPECT_EQ(hits[3].target, 1u);
    EXPECT_EQ(hits[3].ownerImage, 0u);
    EXPECT_EQ(hits[3].ownerRva, 0x1234u);
    EXPECT_EQ(hits[3].ownerDistance, 0x40u);
}

TEST(ResizeReferenceHoldersTest, TheOverlapIsContextOnlyAndTheOwnerSearchIsBounded) {
    const auto images = Images();
    const uintptr_t buffer = 0x5550000;
    std::vector<uint8_t> block(4096, 0);
    Put(block, 0x10, buffer);                 // inside the re-read overlap: not reported again
    Put(block, 0x200, kImageA + 0x10);        // vtable of the object below, still in the overlap
    Put(block, 0x220, buffer);                // reported, owner found in the overlap
    Put(block, 0xC00, buffer);                // no image pointer within the look-back window
    Put(block, 0xC00 - holders::kOwnerLookBackBytes - 8, kImageB);

    holders::Hit hits[4];
    const uintptr_t reportFrom = kBlockBase + 0x218;
    const size_t found = holders::ScanBlock(block.data(), block.size(), kBlockBase, reportFrom, &buffer, 1,
                                            images.data(), images.size(), hits, 4);
    ASSERT_EQ(found, 2u);
    EXPECT_EQ(hits[0].address, kBlockBase + 0x220);
    EXPECT_TRUE(hits[0].ownerFound);
    EXPECT_EQ(hits[0].ownerImage, 0u);
    EXPECT_EQ(hits[1].address, kBlockBase + 0xC00);
    EXPECT_FALSE(hits[1].ownerFound);
}

TEST(ResizeReferenceHoldersTest, CountsBeyondCapacityAndIgnoresMisalignedAndNullValues) {
    const auto images = Images();
    const uintptr_t buffer = 0x5550000;
    std::vector<uint8_t> block(256, 0);
    for (size_t offset = 0; offset < 64; offset += 8) {
        Put(block, offset, buffer);
    }
    Put(block, 0x81, buffer);  // not pointer-aligned: never a stored pointer
    holders::Hit hits[3];
    EXPECT_EQ(holders::ScanBlock(block.data(), block.size(), kBlockBase, kBlockBase, &buffer, 1, images.data(),
                                 images.size(), hits, 3),
              8u);
    const uintptr_t nullTarget = 0;
    EXPECT_EQ(holders::ScanBlock(block.data(), block.size(), kBlockBase, kBlockBase, &nullTarget, 1, images.data(),
                                 images.size(), hits, 3),
              0u);
    EXPECT_EQ(holders::ScanBlock(nullptr, 64, kBlockBase, kBlockBase, &buffer, 1, images.data(), images.size(), hits,
                                 3),
              0u);
}

TEST(ResizeReferenceHoldersTest, FindImageUsesHalfOpenRanges) {
    const auto images = Images();
    EXPECT_EQ(holders::FindImage(kImageA, images.data(), images.size())->index, 0u);
    EXPECT_EQ(holders::FindImage(kImageA + 0xFFFFF, images.data(), images.size())->index, 0u);
    EXPECT_EQ(holders::FindImage(kImageA + 0x100000, images.data(), images.size()), nullptr);
    EXPECT_EQ(holders::FindImage(kImageB + 0x10, images.data(), images.size())->index, 1u);
    EXPECT_EQ(holders::FindImage(0x1000, images.data(), images.size()), nullptr);
    EXPECT_EQ(holders::FindImage(kImageA, nullptr, 0), nullptr);
}

// Reading a write-combined upload mapping from the CPU crawls, a guard page
// would fire, and a holder never keeps its slot in read-only memory.
TEST(ResizeReferenceHoldersTest, OnlyCommittedWritableCachedPrivateOrImageMemoryIsRead) {
    EXPECT_TRUE(holders::IsScannableRegion(MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE));
    EXPECT_TRUE(holders::IsScannableRegion(MEM_COMMIT, PAGE_WRITECOPY, MEM_IMAGE));
    EXPECT_TRUE(holders::IsScannableRegion(MEM_COMMIT, PAGE_EXECUTE_READWRITE, MEM_PRIVATE));

    EXPECT_FALSE(holders::IsScannableRegion(MEM_RESERVE, PAGE_READWRITE, MEM_PRIVATE));
    EXPECT_FALSE(holders::IsScannableRegion(MEM_COMMIT, PAGE_READONLY, MEM_PRIVATE));
    EXPECT_FALSE(holders::IsScannableRegion(MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD, MEM_PRIVATE));
    EXPECT_FALSE(holders::IsScannableRegion(MEM_COMMIT, PAGE_READWRITE | PAGE_WRITECOMBINE, MEM_PRIVATE));
    EXPECT_FALSE(holders::IsScannableRegion(MEM_COMMIT, PAGE_READWRITE | PAGE_NOCACHE, MEM_PRIVATE));
    EXPECT_FALSE(holders::IsScannableRegion(MEM_COMMIT, PAGE_READWRITE, MEM_MAPPED));
}

TEST(ResizeReferenceHoldersTest, OnlyTheFirstRefusedResizeWithForeignReferencesIsScanned) {
    EXPECT_TRUE(holders::ShouldScanFailedResize(holders::kDxgiErrorInvalidCall, true, false));
    EXPECT_FALSE(holders::ShouldScanFailedResize(holders::kDxgiErrorInvalidCall, true, true));
    EXPECT_FALSE(holders::ShouldScanFailedResize(holders::kDxgiErrorInvalidCall, false, false));
    EXPECT_FALSE(holders::ShouldScanFailedResize(S_OK, true, false));
    EXPECT_FALSE(holders::ShouldScanFailedResize(E_OUTOFMEMORY, true, false));
}

// The live walk over this test process: the heap object's three slots are found
// (the caller's own array on this thread's stack adds three more; register
// spills may add a few, which is why this is a lower bound).
TEST(ResizeReferenceHoldersTest, TheLiveScanFindsAHeapHolderInThisProcess) {
    auto* holder = new volatile uintptr_t[8]();
    holder[0] = reinterpret_cast<uintptr_t>(&holders::FindImage);
    // Values no other memory of this process holds; built at run time so the
    // constants never sit in the test image's own data.
    uintptr_t seed = reinterpret_cast<uintptr_t>(holder) ^ 0x5A5A000000000000ull;
    void* buffers[3] = {};
    for (int i = 0; i < 3; ++i) {
        buffers[i] = reinterpret_cast<void*>(seed + static_cast<uintptr_t>(i) * 0x1000);
        holder[2 + i] = reinterpret_cast<uintptr_t>(buffers[i]);
    }
    const size_t slots = LogBackBufferReferenceHolders(buffers, 3, "unit-test");
    delete[] holder;
    EXPECT_GE(slots, 3u);
}

// The scan holds a game's resizing thread; a process with a huge working set must not turn a
// diagnostic into a multi-minute freeze.
TEST(ResizeReferenceHoldersTest, TheScanStopsAtItsTimeBudget) {
    EXPECT_FALSE(holders::IsScanBudgetExhausted(0));
    EXPECT_FALSE(holders::IsScanBudgetExhausted(holders::kScanBudgetUs - 1));
    EXPECT_TRUE(holders::IsScanBudgetExhausted(holders::kScanBudgetUs));
    EXPECT_TRUE(holders::IsScanBudgetExhausted(holders::kScanBudgetUs * 10));
    // Normal scans take a second or two; the budget must not cut those.
    EXPECT_GE(holders::kScanBudgetUs, 2'500'000);
}

TEST(ResizeReferenceHoldersTest, TheWalkChecksTheBudgetPerChunkAndReportsACut) {
    const std::string scan = ce::test_source::ReadFile(std::filesystem::current_path() /
                                                       "hook/present/resize_reference_holders.cpp");
    ASSERT_FALSE(scan.empty());
    const size_t chunkLoop = scan.find("for (uintptr_t chunkStart = regionBase;");
    const size_t budget = scan.find("IsScanBudgetExhausted(elapsedUs())", chunkLoop);
    const size_t read = scan.find("ReadProcessMemory(", chunkLoop);
    ASSERT_NE(chunkLoop, std::string::npos);
    ASSERT_NE(budget, std::string::npos);
    ASSERT_NE(read, std::string::npos);
    EXPECT_LT(budget, read) << "the budget is checked before each chunk is read";
    EXPECT_NE(scan.find("CUT at the time budget"), std::string::npos)
        << "a truncated scan must say so, or its slot list reads as complete";
}

// Wired into the failed-resize diagnostics, once, after the reference probe.
TEST(ResizeReferenceHoldersTest, TheFailedResizeDiagnosticsRunTheScanOnce) {
    const std::string resize = ReadSource("hook/present/dxgi_shared_resize.cpp");
    ASSERT_FALSE(resize.empty());
    const size_t begin = resize.find("void EndD3D12ResizeDiagnostics(");
    ASSERT_NE(begin, std::string::npos);
    const std::string body = resize.substr(begin, resize.find("\n}\n", begin) - begin);
    const size_t logged = body.find("backBufferRefsHeldByOthers before=");
    const size_t gate = body.find("ShouldScanFailedResize(hr, heldAfterFailure");
    const size_t once = body.find("s_holdersScanned.exchange(true");
    const size_t scan = body.find("LogBackBufferReferenceHolders(buffers, bufferCount, source)");
    ASSERT_NE(logged, std::string::npos);
    ASSERT_NE(gate, std::string::npos);
    ASSERT_NE(once, std::string::npos);
    ASSERT_NE(scan, std::string::npos);
    EXPECT_LT(logged, gate);
    EXPECT_LT(once, scan);
}
