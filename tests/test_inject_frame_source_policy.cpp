#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../common/inject_frame_source_policy.h"
#include "source_fragment_reader.h"

using ce::inject_frame_source::Admission;
using ce::inject_frame_source::Classify;
using ce::inject_frame_source::ParentCheck;
using ce::inject_frame_source::SplitRendererVerificationCache;

namespace {

// Portal RTX session 20260929_031827: hl2.exe is the injected 32-bit D3D9
// client and session source; NvRemixBridge.exe is its direct child and owns
// the Vulkan swapchain the CE layer captures.
constexpr uint32_t kClientPid = 12472;    // hl2.exe
constexpr uint32_t kRendererPid = 17220;  // NvRemixBridge.exe
constexpr uint32_t kUnrelatedPid = 4242;

std::string ReadProjectSource(const std::filesystem::path& relativePath) {
    const std::string source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
    EXPECT_FALSE(source.empty()) << relativePath.string();
    return source;
}

}  // namespace

TEST(InjectFrameSourcePolicyTest, SessionOwnerAndUnstampedSlotsAreAdmitted) {
    EXPECT_EQ(Classify(kClientPid, kClientPid, 0, 0), Admission::kSessionSource);
    EXPECT_EQ(Classify(0, kClientPid, 0, 0), Admission::kSessionSource);
    EXPECT_EQ(Classify(kClientPid, 0, 0, 0), Admission::kSessionSource);
    // A direct Vulkan renderer claims itself in both halves.
    EXPECT_EQ(Classify(kClientPid, kClientPid, kClientPid, kClientPid), Admission::kSessionSource);
}

TEST(InjectFrameSourcePolicyTest, ClaimedSplitRendererIsACandidate) {
    // Before the fix every one of these frames was dropped as a PID mismatch and
    // the recording never left "preparing".
    EXPECT_EQ(Classify(kRendererPid, kClientPid, kRendererPid, kClientPid), Admission::kSplitRendererCandidate);
}

TEST(InjectFrameSourcePolicyTest, WritersTheClaimDoesNotVouchForAreForeign) {
    // No claim at all: nothing links the writer to the session.
    EXPECT_EQ(Classify(kRendererPid, kClientPid, 0, 0), Admission::kForeign);
    // The claim belongs to another process tree (a previous game's renderer).
    EXPECT_EQ(Classify(kRendererPid, kClientPid, kRendererPid, kUnrelatedPid), Admission::kForeign);
    // The claim names a different renderer than the slot's writer.
    EXPECT_EQ(Classify(kUnrelatedPid, kClientPid, kRendererPid, kClientPid), Admission::kForeign);
    // A direct renderer's self-claim never vouches for a different writer.
    EXPECT_EQ(Classify(kRendererPid, kClientPid, kRendererPid, kRendererPid), Admission::kForeign);
    // Halves swapped: the session source cannot be its own child's renderer.
    EXPECT_EQ(Classify(kRendererPid, kClientPid, kClientPid, kRendererPid), Admission::kForeign);
}

TEST(InjectFrameSourcePolicyTest, VerificationRunsOncePerPairAndCachesTheVerdict) {
    SplitRendererVerificationCache cache;
    int checks = 0;
    auto directChild = [&](uint32_t renderer, uint32_t client) {
        ++checks;
        EXPECT_EQ(renderer, kRendererPid);
        EXPECT_EQ(client, kClientPid);
        return ParentCheck::kDirectChild;
    };

    const auto first = cache.Verify(kRendererPid, kClientPid, directChild);
    EXPECT_TRUE(first.admitted);
    EXPECT_TRUE(first.ranCheck);
    EXPECT_EQ(first.check, ParentCheck::kDirectChild);

    for (int frame = 0; frame < 100; ++frame) {
        const auto next = cache.Verify(kRendererPid, kClientPid, directChild);
        EXPECT_TRUE(next.admitted);
        EXPECT_FALSE(next.ranCheck);
    }
    EXPECT_EQ(checks, 1);
}

TEST(InjectFrameSourcePolicyTest, RejectedPairStaysRejectedUntilThePairChanges) {
    SplitRendererVerificationCache cache;
    int checks = 0;
    auto notChild = [&](uint32_t, uint32_t) {
        ++checks;
        return ParentCheck::kNotDirectChild;
    };
    EXPECT_FALSE(cache.Verify(kRendererPid, kClientPid, notChild).admitted);
    EXPECT_FALSE(cache.Verify(kRendererPid, kClientPid, notChild).admitted);
    EXPECT_EQ(checks, 1);

    // A restarted renderer is a new pair and gets its own check.
    auto directChild = [&](uint32_t, uint32_t) {
        ++checks;
        return ParentCheck::kDirectChild;
    };
    const auto restarted = cache.Verify(kRendererPid + 4, kClientPid, directChild);
    EXPECT_TRUE(restarted.ranCheck);
    EXPECT_TRUE(restarted.admitted);
    EXPECT_EQ(checks, 2);
}

TEST(InjectFrameSourcePolicyTest, UnavailableSnapshotIsRetriedInsteadOfLatchingARejection) {
    SplitRendererVerificationCache cache;
    int checks = 0;
    ParentCheck answer = ParentCheck::kUnavailable;
    auto check = [&](uint32_t, uint32_t) {
        ++checks;
        return answer;
    };
    const auto failed = cache.Verify(kRendererPid, kClientPid, check);
    EXPECT_FALSE(failed.admitted);
    EXPECT_TRUE(failed.ranCheck);
    EXPECT_EQ(failed.check, ParentCheck::kUnavailable);

    answer = ParentCheck::kDirectChild;
    const auto retried = cache.Verify(kRendererPid, kClientPid, check);
    EXPECT_TRUE(retried.ranCheck);
    EXPECT_TRUE(retried.admitted);
    EXPECT_EQ(checks, 2);
}

TEST(InjectFrameSourcePolicyTest, DegeneratePairsNeverReachTheProcessCheck) {
    SplitRendererVerificationCache cache;
    auto mustNotRun = [](uint32_t, uint32_t) {
        ADD_FAILURE() << "degenerate pair reached the process check";
        return ParentCheck::kDirectChild;
    };
    EXPECT_FALSE(cache.Verify(0, kClientPid, mustNotRun).admitted);
    EXPECT_FALSE(cache.Verify(kRendererPid, 0, mustNotRun).admitted);
    EXPECT_FALSE(cache.Verify(kClientPid, kClientPid, mustNotRun).admitted);
}

TEST(InjectFrameSourcePolicyTest, MediaInjectThreadAdmitsVerifiedSplitRenderers) {
    const std::string source = ReadProjectSource("captureengine/media_main_threads_inject.cpp");

    const size_t pin = source.find("// Pin the frame's source PID to the process that owns this capture session.");
    ASSERT_NE(pin, std::string::npos);
    const size_t claim = source.find("runtimeState.vulkanLayerClaim.load(std::memory_order_acquire)", pin);
    const size_t classify = source.find("ce::inject_frame_source::Classify(", pin);
    const size_t verify = source.find("splitRendererVerification.Verify(slot.sourcePid, sessionSourcePid,", pin);
    const size_t drop = source.find("if (!sourceAdmitted) {", pin);
    const size_t assign = source.find("qf.sourcePid = (slot.sourcePid != 0) ? slot.sourcePid : sessionSourcePid;", pin);
    ASSERT_NE(claim, std::string::npos);
    ASSERT_NE(classify, std::string::npos);
    ASSERT_NE(verify, std::string::npos);
    ASSERT_NE(drop, std::string::npos);
    ASSERT_NE(assign, std::string::npos);
    EXPECT_LT(claim, classify);
    EXPECT_LT(classify, verify);
    EXPECT_LT(verify, drop);
    EXPECT_LT(drop, assign);
    // The raw PID comparison that rejected every NvRemixBridge.exe frame must not return.
    EXPECT_EQ(source.find("slot.sourcePid != sessionSourcePid) {", pin), std::string::npos);

    // The split renderer owns the handles, but not the window: the cursor is
    // mapped through the session source's client area.
    EXPECT_NE(source.find("const DWORD cursorWindowPid = sessionSourcePid != 0 ? sessionSourcePid : qf.sourcePid;"),
              std::string::npos);
    EXPECT_NE(source.find("GetMainWindowForProcess(cursorWindowPid)"), std::string::npos);

    // The verification re-proves the layer's direct-child topology.
    EXPECT_NE(source.find("entry.th32ParentProcessID == clientPid"), std::string::npos);
}
