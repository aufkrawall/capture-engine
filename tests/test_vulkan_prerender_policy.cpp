#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

#include "hook/vulkan_layer/vulkan_prerender_policy.h"
#include "source_fragment_reader.h"

// Coverage for the Vulkan layer's `cpu_prerender_limit` routes: the marker the
// present hook appends to the producer queue, and the producer-submit route a
// cross-thread producer needs because a VkQueue is externally synchronized.

namespace {

std::string ReadProjectSource(const char* relativePath) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / relativePath;
    if (!fs::exists(source))
        return {};
    return ce::test_source::ReadFile(source);
}

}  // namespace

TEST(VulkanPrerenderPolicyTest, MovesCrossThreadQueuePacingToTheProducerSubmit) {
    using ce::vulkan_prerender_policy::ShouldPaceOnProducerSubmit;

    EXPECT_TRUE(ShouldPaceOnProducerSubmit(8184, 27228));
    EXPECT_FALSE(ShouldPaceOnProducerSubmit(8184, 8184));
    EXPECT_FALSE(ShouldPaceOnProducerSubmit(0, 8184));
    EXPECT_FALSE(ShouldPaceOnProducerSubmit(8184, 0));
}

// Strange Brigade Vulkan (session 20261018_172054): the game presents from its
// one graphics queue, which is also the producer, while a second thread's
// occasional submits latch async-present. The latch alone kept the marker off,
// so cpu_prerender_limit=1 left PC latency at 55 ms.
TEST(VulkanPrerenderPolicyTest, PresentQueueMarkerIgnoresTheAsyncPresentLatch) {
    using ce::vulkan_prerender_policy::CanAppendMarkerAtPresent;
    constexpr uint32_t kWorkerThread = 7820;
    constexpr uint32_t kPresentThread = 27076;

    EXPECT_TRUE(CanAppendMarkerAtPresent(true, true, kWorkerThread, kPresentThread))
        << "vkQueuePresentKHR holds the present queue for the call, whoever else submits to it";
    EXPECT_TRUE(CanAppendMarkerAtPresent(true, false, kPresentThread, kPresentThread));
    EXPECT_TRUE(CanAppendMarkerAtPresent(true, true, 0, kPresentThread));

    // A separate producer queue (compute present) keeps the ownership rule.
    EXPECT_TRUE(CanAppendMarkerAtPresent(false, false, kPresentThread, kPresentThread));
    EXPECT_TRUE(CanAppendMarkerAtPresent(false, false, 0, kPresentThread));
    EXPECT_FALSE(CanAppendMarkerAtPresent(false, false, kWorkerThread, kPresentThread))
        << "another thread's queue is paced from that thread's submit wrapper";
    EXPECT_FALSE(CanAppendMarkerAtPresent(false, true, kPresentThread, kPresentThread));
}

TEST(VulkanPrerenderPolicyTest, RearmsProducerRouteLearningOncePerLateThreadSplit) {
    using ce::vulkan_prerender_policy::ShouldRearmProducerRouteLearning;
    constexpr uint32_t kRenderThread = 26380;
    constexpr uint32_t kPresentThread = 24196;

    EXPECT_TRUE(ShouldRearmProducerRouteLearning(false, false, false, kRenderThread, kPresentThread));
    EXPECT_FALSE(ShouldRearmProducerRouteLearning(false, false, true, kRenderThread, kPresentThread))
        << "one split re-arms once, so a route that cannot be learned does not keep learning";
    EXPECT_FALSE(ShouldRearmProducerRouteLearning(true, false, false, kRenderThread, kPresentThread))
        << "an armed producer-submit route already paces from the render thread";
    EXPECT_FALSE(ShouldRearmProducerRouteLearning(false, true, false, kRenderThread, kPresentThread))
        << "an open learning window sees the split on its own";
    EXPECT_FALSE(ShouldRearmProducerRouteLearning(false, false, false, kPresentThread, kPresentThread))
        << "a same-thread producer is paced at present";
    EXPECT_FALSE(ShouldRearmProducerRouteLearning(false, false, false, 0, kPresentThread));
}

TEST(VulkanPrerenderPolicyTest, RelearnsOnlyForAKnownLiveQueueFamilyMove) {
    using ce::vulkan_prerender_policy::ShouldRelearnPresentTopology;
    constexpr uint32_t kUnknownQueueFamily = std::numeric_limits<uint32_t>::max();

    EXPECT_FALSE(ShouldRelearnPresentTopology(kUnknownQueueFamily, 0));
    EXPECT_FALSE(ShouldRelearnPresentTopology(0, kUnknownQueueFamily));
    EXPECT_FALSE(ShouldRelearnPresentTopology(0, 0));
    EXPECT_TRUE(ShouldRelearnPresentTopology(0, 2));
}

// A 1024-marker window logged ~2 lines/s in Strange Brigade's ~2000 fps menu
// (session 20261018_173529); the summary now follows wall-clock time.
TEST(VulkanPrerenderPolicyTest, SummaryWindowIsTenSecondsAtAnyFrameRate) {
    using ce::vulkan_prerender_policy::IsSummaryWindowDue;
    using ce::vulkan_prerender_policy::kSummaryWindowUs;
    constexpr uint64_t kStartUs = 5'000'000;

    EXPECT_EQ(kSummaryWindowUs, 10'000'000u);
    EXPECT_FALSE(IsSummaryWindowDue(0, kStartUs + kSummaryWindowUs)) << "no window has started yet";
    EXPECT_FALSE(IsSummaryWindowDue(kStartUs, kStartUs));
    // 2000 fps for 1024 markers is ~0.5 s: far from due.
    EXPECT_FALSE(IsSummaryWindowDue(kStartUs, kStartUs + 512'000));
    EXPECT_FALSE(IsSummaryWindowDue(kStartUs, kStartUs + kSummaryWindowUs - 1));
    EXPECT_TRUE(IsSummaryWindowDue(kStartUs, kStartUs + kSummaryWindowUs));
    // A minimized game that stops presenting reports its long window on the
    // first marker after restore.
    EXPECT_TRUE(IsSummaryWindowDue(kStartUs, kStartUs + 37'528'000));
    EXPECT_FALSE(IsSummaryWindowDue(kStartUs, kStartUs - 1)) << "a clock that went backwards is not due";
}

TEST(VulkanPrerenderSourceTest, PresentHookRoutesTheMarkerThroughThePolicy) {
    const std::string present = ReadProjectSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    const std::string topology = ReadProjectSource("hook/vulkan_layer/vulkan_prerender_topology.cpp");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(topology.empty());

    // The async-present latch must gate neither the present-queue marker nor
    // the split re-arm for a separate producer queue.
    EXPECT_EQ(present.find("!paceOnProducerSubmit && !asyncPresentDetected"), std::string::npos);
    EXPECT_NE(present.find("const bool markerOnPresentQueue = prerenderQueue == queue;"), std::string::npos);
    const size_t observe = present.find("ObservePrerenderThreadSplit(sd, prerenderQueue, producerThreadId)");
    const size_t decide = present.find("CanAppendMarkerAtPresent(markerOnPresentQueue, asyncPresentDetected");
    const size_t apply = present.find("ApplyPrerenderLimitVulkan(queueDevice, prerenderQueue");
    ASSERT_NE(observe, std::string::npos);
    ASSERT_NE(decide, std::string::npos);
    ASSERT_NE(apply, std::string::npos);
    EXPECT_LT(observe, decide);
    EXPECT_LT(decide, apply);

    EXPECT_NE(topology.find("ShouldRearmProducerRouteLearning("), std::string::npos);
    EXPECT_NE(topology.find("prerenderSplitRelearned.exchange(true"), std::string::npos);
    EXPECT_NE(topology.find("m_LearnPrerenderBoundaries.store(true"), std::string::npos);
    EXPECT_NE(topology.find("re-learning the producer-submit boundary"), std::string::npos);
    EXPECT_NE(topology.find("re-learning ended without a producer-submit boundary"), std::string::npos);
}

// Before this, every Vulkan perf row reported prerender_wait_us=0, so a session
// could not show whether the configured depth ever held a thread back.
TEST(VulkanPrerenderSourceTest, MarkerWaitReachesThePerfRowAndASummary) {
    const std::string present = ReadProjectSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    const std::string marker = ReadProjectSource("hook/vulkan_layer/vulkan_layer_internal.h");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(marker.empty());

    EXPECT_NE(marker.find("inline uint32_t ApplyPrerenderLimitVulkan("), std::string::npos);
    EXPECT_NE(marker.find("Vulkan Prerender: summary queue=%p route=%s"), std::string::npos);
    EXPECT_NE(marker.find("IsSummaryWindowDue(state.statWindowStartUs, nowUs)"), std::string::npos)
        << "one summary per wall-clock window, not per marker count";
    EXPECT_EQ(marker.find("state.statMarkers < 1024"), std::string::npos);
    EXPECT_NE(marker.find("state.statWindowStartUs = nowUs;"), std::string::npos);
    EXPECT_NE(marker.find("vulkan_layer_g_ProducerPrerenderWaitUs.fetch_add"), std::string::npos);
    EXPECT_NE(present.find("perfMetrics.prerenderWaitUs =\n"), std::string::npos);
    EXPECT_NE(present.find("vulkan_layer_g_ProducerPrerenderWaitUs.exchange(0"), std::string::npos)
        << "producer-submit waits on the render thread must reach the next present's row";
}
