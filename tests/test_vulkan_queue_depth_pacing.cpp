#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/vulkan_layer/vulkan_queue_depth_pacing_policy.h"
#include "source_fragment_reader.h"

namespace policy = ce::vulkan_queue_depth_pacing;

namespace {

policy::DeviceInput SupportedDevice(int32_t backbufferCount) {
    policy::DeviceInput in;
    in.configuredBackbufferCount = backbufferCount;
    in.presentIdExtensionAvailable = true;
    in.presentWaitExtensionAvailable = true;
    in.presentIdFeatureSupported = true;
    in.presentWaitFeatureSupported = true;
    in.applicationEnabledSwapchain = true;
    return in;
}

policy::PresentInput PacedPresent() {
    policy::PresentInput in;
    in.deviceEnabled = true;
    in.configuredBackbufferCount = 2;
    in.swapchainImageCount = 3;
    in.presentSwapchainCount = 1;
    return in;
}

std::string ReadSource(const char* relative) {
    return ce::test_source::ReadFile(std::filesystem::current_path() / relative);
}

}  // namespace

// Strange Brigade Vulkan (logs/20261019_022104): backbuffer_count=2, the game asked for 3 images and
// CE rightly kept them, so the setting did nothing. The queue depth is now paced instead.
TEST(VulkanQueueDepthPacingTest, DeviceEnablesPresentWaitOnlyWhenCeOwnsIt) {
    EXPECT_EQ(policy::DecideDeviceEnablement(SupportedDevice(2)), policy::DeviceDecision::kEnable);
    EXPECT_EQ(policy::DecideDeviceEnablement(SupportedDevice(-1)), policy::DeviceDecision::kNotConfigured);
    EXPECT_EQ(policy::DecideDeviceEnablement(SupportedDevice(1)), policy::DeviceDecision::kNotConfigured);

    auto in = SupportedDevice(2);
    in.applicationEnabledSwapchain = false;
    EXPECT_EQ(policy::DecideDeviceEnablement(in), policy::DeviceDecision::kNoSwapchain);

    // A game that manages its own present ids or waits (DXVK) keeps them; CE never waits beside it.
    in = SupportedDevice(2);
    in.applicationEnabledPresentId = true;
    EXPECT_EQ(policy::DecideDeviceEnablement(in), policy::DeviceDecision::kApplicationOwned);
    in = SupportedDevice(2);
    in.applicationEnabledPresentWait = true;
    EXPECT_EQ(policy::DecideDeviceEnablement(in), policy::DeviceDecision::kApplicationOwned);

    in = SupportedDevice(2);
    in.applicationEnabledPresentMetering = true;
    EXPECT_EQ(policy::DecideDeviceEnablement(in), policy::DeviceDecision::kMeteredFrameGenerator);

    for (int missing = 0; missing < 4; ++missing) {
        in = SupportedDevice(2);
        bool* fields[] = {&in.presentIdExtensionAvailable, &in.presentWaitExtensionAvailable,
                          &in.presentIdFeatureSupported, &in.presentWaitFeatureSupported};
        *fields[missing] = false;
        EXPECT_EQ(policy::DecideDeviceEnablement(in), policy::DeviceDecision::kUnsupported) << missing;
    }

    // A second structure of the same sType in pNext is invalid.
    in = SupportedDevice(2);
    in.applicationChainHasPresentFeatures = true;
    EXPECT_EQ(policy::DecideDeviceEnablement(in), policy::DeviceDecision::kFeatureStructureConflict);
}

TEST(VulkanQueueDepthPacingTest, PresentIsPacedOnlyWhereTheImageCountCannotCarryTheDepth) {
    EXPECT_EQ(policy::DecidePresent(PacedPresent()), policy::PresentDecision::kPace);

    auto in = PacedPresent();
    in.configuredBackbufferCount = -1;
    EXPECT_EQ(policy::DecidePresent(in), policy::PresentDecision::kNotConfigured);
    in = PacedPresent();
    in.deviceEnabled = false;
    EXPECT_EQ(policy::DecidePresent(in), policy::PresentDecision::kDeviceNotEnabled);
    // A metered frame generator places its generated images itself.
    in = PacedPresent();
    in.frameGenerationOwnsPlacement = true;
    EXPECT_EQ(policy::DecidePresent(in), policy::PresentDecision::kFrameGeneration);
    in = PacedPresent();
    in.applicationSuppliedPresentId = true;
    EXPECT_EQ(policy::DecidePresent(in), policy::PresentDecision::kApplicationPresentId);
    in = PacedPresent();
    in.presentSwapchainCount = 2;
    EXPECT_EQ(policy::DecidePresent(in), policy::PresentDecision::kMultipleSwapchains);
    // backbuffer_count at or above the image count: the images already bound the queue.
    in = PacedPresent();
    in.configuredBackbufferCount = 3;
    EXPECT_EQ(policy::DecidePresent(in), policy::PresentDecision::kImageCountCarriesDepth);
    in.swapchainImageCount = 4;
    EXPECT_EQ(policy::DecidePresent(in), policy::PresentDecision::kPace);
}

TEST(VulkanQueueDepthPacingTest, WaitKeepsAtMostBackbufferCountMinusOnePresentsOutstanding) {
    // backbuffer_count=2: one outstanding present, so present k waits for k-1.
    EXPECT_EQ(policy::WaitTargetFor(1, 2), 0u);
    EXPECT_EQ(policy::WaitTargetFor(2, 2), 1u);
    EXPECT_EQ(policy::WaitTargetFor(3, 2), 2u);
    // backbuffer_count=3: two outstanding.
    EXPECT_EQ(policy::WaitTargetFor(2, 3), 0u);
    EXPECT_EQ(policy::WaitTargetFor(3, 3), 1u);
    EXPECT_EQ(policy::WaitTargetFor(10, 6), 5u);
    EXPECT_EQ(policy::WaitTargetFor(10, 1), 0u);
    EXPECT_EQ(policy::WaitTargetFor(10, -1), 0u);
}

TEST(VulkanQueueDepthPacingTest, MissedCeilingDegradesToNonBlockingChecksUntilAPresentCompletes) {
    EXPECT_EQ(policy::ClassifyWaitResult(VK_SUCCESS), policy::WaitOutcome::kCompleted);
    EXPECT_EQ(policy::ClassifyWaitResult(VK_SUBOPTIMAL_KHR), policy::WaitOutcome::kCompleted);
    EXPECT_EQ(policy::ClassifyWaitResult(VK_TIMEOUT), policy::WaitOutcome::kTimedOut);
    EXPECT_EQ(policy::ClassifyWaitResult(VK_ERROR_OUT_OF_DATE_KHR), policy::WaitOutcome::kSwapchainUnusable);
    EXPECT_EQ(policy::ClassifyWaitResult(VK_ERROR_SURFACE_LOST_KHR), policy::WaitOutcome::kSwapchainUnusable);
    EXPECT_EQ(policy::ClassifyWaitResult(VK_ERROR_DEVICE_LOST), policy::WaitOutcome::kFailed);

    // A minimized window: presents stop completing, one wait pays the ceiling, then no further stalls.
    bool degraded = false;
    EXPECT_EQ(policy::WaitTimeoutNs(degraded), policy::kWaitCeilingNs);
    degraded = policy::NextDegradedState(degraded, policy::WaitOutcome::kTimedOut);
    EXPECT_TRUE(degraded);
    EXPECT_EQ(policy::WaitTimeoutNs(degraded), 0u);
    degraded = policy::NextDegradedState(degraded, policy::WaitOutcome::kTimedOut);
    EXPECT_TRUE(degraded) << "a non-blocking miss keeps checking without blocking";
    degraded = policy::NextDegradedState(degraded, policy::WaitOutcome::kSwapchainUnusable);
    EXPECT_TRUE(degraded);
    degraded = policy::NextDegradedState(degraded, policy::WaitOutcome::kCompleted);
    EXPECT_FALSE(degraded) << "presents complete again: blocking waits resume";
    EXPECT_FALSE(policy::NextDegradedState(false, policy::WaitOutcome::kFailed));
}

TEST(VulkanQueueDepthPacingTest, CeilingStaysAboveTheSlowestHealthyWait) {
    // 24 Hz is the slowest mode Windows drives; backbuffer_count accepts at most 6.
    const uint64_t slowestHealthyWaitNs = 6ull * 41'700'000ull;
    EXPECT_GT(policy::kWaitCeilingNs, slowestHealthyWaitNs);
}

TEST(VulkanQueueDepthPacingTest, NeverWaitsForAPresentTheWsiRejected) {
    policy::SwapchainState state;
    // backbuffer_count=2: presents 1..4 accepted, 5 rejected as out of date during a resize.
    for (uint64_t id = 1; id <= 5; ++id)
        policy::NoteQueuedPresent(state, id, id != 5);
    EXPECT_TRUE(policy::WasQueued(state, policy::WaitTargetFor(5, 2)));
    EXPECT_FALSE(policy::WasQueued(state, policy::WaitTargetFor(6, 2))) << "present 5 never reaches the display";
    policy::NoteQueuedPresent(state, 6, true);
    EXPECT_TRUE(policy::WasQueued(state, policy::WaitTargetFor(7, 2)));
    EXPECT_FALSE(policy::WasQueued(state, 0));
    // The ring only answers for its own id: a slot reused 8 presents later is a different present.
    policy::NoteQueuedPresent(state, 14, true);
    EXPECT_FALSE(policy::WasQueued(state, 6));
    EXPECT_TRUE(policy::WasQueued(state, 14));
    for (uint64_t id = 7; id <= 13; ++id)
        policy::NoteQueuedPresent(state, id, true);
    // The deepest target (backbuffer_count=6) is five presents back, inside the ring.
    EXPECT_TRUE(policy::WasQueued(state, policy::WaitTargetFor(14, 6)));
    EXPECT_GT(policy::kQueuedIdRingSize, 5u);
}

TEST(VulkanQueueDepthPacingSourceTest, DeviceCreationEnablesTheExtensionsAndNeverFailsBecauseOfThem) {
    const std::string hooks = ReadSource("hook/vulkan_layer/vulkan_layer_hooks.cpp");
    ASSERT_FALSE(hooks.empty());
    const size_t create = hooks.find("VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateDevice(");
    ASSERT_NE(create, std::string::npos);
    const size_t prepare = hooks.find("PrepareQueueDepthPacingForDevice(", create);
    const size_t call = hooks.find("result = create_fn(physicalDevice, &modifiedCreateInfo", create);
    const size_t withdraw = hooks.find("WithdrawQueueDepthPacingFromCreateInfo(extensions, modifiedCreateInfo", create);
    const size_t populate = hooks.find("PopulateDeviceDispatch(dispatch, *pDevice, gdpa);", create);
    const size_t finish = hooks.find("FinishQueueDepthPacingForDevice(dispatch, *pDevice, gdpa, queueDepthSetup);", create);
    ASSERT_NE(prepare, std::string::npos);
    ASSERT_NE(call, std::string::npos);
    ASSERT_NE(withdraw, std::string::npos);
    ASSERT_NE(populate, std::string::npos);
    ASSERT_NE(finish, std::string::npos);
    EXPECT_LT(prepare, call);
    EXPECT_LT(call, withdraw) << "a refused create is retried without the extensions";
    EXPECT_LT(populate, finish) << "the wait entry point is resolved from the created device";

    const std::string build = ReadSource("tools/build/build_vulkan_layer.py");
    EXPECT_NE(build.find("\"vulkan_queue_depth_pacing.cpp\""), std::string::npos);
}

TEST(VulkanQueueDepthPacingSourceTest, PresentWaitsAfterTheLimiterRightBeforeTheDownCall) {
    const std::string present = ReadSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    ASSERT_FALSE(present.empty());
    const size_t limiter = present.find("g_SharedFpsLimiter.Apply(true,");
    const size_t pace = present.find("PaceVulkanPresentQueueDepth(disp, queueDevice, sd, pPresentInfo,");
    const size_t copy = present.find("presentInfoCopy.pNext = finalPresentChain;");
    const size_t downCall = present.find("res = disp->fp_vkQueuePresentKHR(queue, (pPresentInfo && modified)");
    const size_t note = present.find("NoteVulkanQueueDepthPresentResult(sd, queueDepthTag, res);");
    ASSERT_NE(limiter, std::string::npos);
    ASSERT_NE(pace, std::string::npos);
    ASSERT_NE(copy, std::string::npos);
    ASSERT_NE(downCall, std::string::npos);
    ASSERT_NE(note, std::string::npos);
    EXPECT_LT(limiter, pace);
    EXPECT_LT(pace, copy) << "the CE present id must be in the chain the down-call receives";
    EXPECT_LT(copy, downCall);
    EXPECT_LT(downCall, note);

    const std::string pacing = ReadSource("hook/vulkan_layer/vulkan_queue_depth_pacing.cpp");
    ASSERT_FALSE(pacing.empty());
    const size_t queued = pacing.find("policy::WasQueued(st, target)");
    const size_t wait = pacing.find("dispatch->fp_vkWaitForPresentKHR(device, swapchain->swapchain, target,");
    ASSERT_NE(queued, std::string::npos);
    ASSERT_NE(wait, std::string::npos);
    EXPECT_LT(queued, wait);
    EXPECT_NE(pacing.find("policy::WaitTimeoutNs(st.degraded)"), std::string::npos)
        << "every wait is bounded by the ceiling or non-blocking";
}

// Strange Brigade Vulkan recreates its swapchain every 3-5 s and on every focus change or minimize
// (logs/20261019_124700, _125351), so a per-swapchain 10 s window never printed. The window now
// follows the presentation across swapchain generations.
TEST(VulkanQueueDepthPacingTest, StatisticsWindowSurvivesSwapchainRecreation) {
    constexpr uint64_t kStartUs = 1'000'000;
    policy::WindowStats first;
    EXPECT_FALSE(policy::IsWindowDue(first, kStartUs + policy::kWindowUs)) << "a window that never started";
    first.startUs = kStartUs;
    first.presents = 400;
    first.waits = 399;
    first.waitUs = 2'500'000;
    first.maxWaitUs = 9'000;
    first.timeouts = 1;
    first.totalTimeouts = 1;

    policy::WindowStats second;
    ASSERT_TRUE(policy::CarryWindow(second, first));
    EXPECT_EQ(second.startUs, kStartUs) << "the window keeps running instead of restarting per swapchain";
    EXPECT_EQ(second.generations, 2u);
    EXPECT_EQ(second.presents, 400u);
    EXPECT_EQ(second.waits, 399u);
    EXPECT_EQ(second.totalTimeouts, 1u);
    EXPECT_FALSE(policy::IsWindowDue(second, kStartUs + 4'000'000));
    EXPECT_TRUE(policy::IsWindowDue(second, kStartUs + policy::kWindowUs));

    policy::WindowStats third;
    ASSERT_TRUE(policy::CarryWindow(third, second));
    EXPECT_EQ(third.generations, 3u);

    // Nothing to carry from a swapchain that never paced; the successor keeps its own window.
    policy::WindowStats untouched;
    untouched.presents = 7;
    EXPECT_FALSE(policy::CarryWindow(untouched, policy::WindowStats{}));
    EXPECT_EQ(untouched.presents, 7u);
}

TEST(VulkanQueueDepthPacingTest, LoggedWindowRestartsButKeepsSessionTotals) {
    policy::WindowStats w;
    w.startUs = 5;
    w.presents = 1440;
    w.waits = 1439;
    w.timeouts = 2;
    w.generations = 4;
    w.totalTimeouts = 3;
    w.totalRecoveries = 2;
    policy::ResetWindow(w, 10'000'005);
    EXPECT_EQ(w.startUs, 10'000'005u);
    EXPECT_EQ(w.presents, 0u);
    EXPECT_EQ(w.waits, 0u);
    EXPECT_EQ(w.timeouts, 0u);
    EXPECT_EQ(w.generations, 1u);
    EXPECT_EQ(w.totalTimeouts, 3u);
    EXPECT_EQ(w.totalRecoveries, 2u);
    // A clock that went backwards never makes a window due.
    EXPECT_FALSE(policy::IsWindowDue(w, 4));
}

TEST(VulkanQueueDepthPacingSourceTest, DestroyHandsTheOpenWindowToTheNextSwapchain) {
    const std::string pacing = ReadSource("hook/vulkan_layer/vulkan_queue_depth_pacing.cpp");
    const std::string state = ReadSource("hook/vulkan_layer/vulkan_layer_state.cpp");
    const std::string swapchain = ReadSource("hook/vulkan_layer/vulkan_layer_swapchain.cpp");
    ASSERT_FALSE(pacing.empty());
    ASSERT_FALSE(state.empty());
    ASSERT_FALSE(swapchain.empty());

    // Every unregister path (destroy and oldSwapchain retirement) goes through UnregisterSwapchain.
    const size_t unregister = state.find("void VulkanLayerState::UnregisterSwapchain(");
    ASSERT_NE(unregister, std::string::npos);
    const size_t handOver = state.find("HandOverVulkanQueueDepthWindow(*it->second);", unregister);
    const size_t release = state.find("delete it->second;", unregister);
    ASSERT_NE(handOver, std::string::npos);
    ASSERT_NE(release, std::string::npos);
    EXPECT_LT(handOver, release) << "the state is read before it is freed";
    const size_t window = swapchain.find("sd->window = VulkanLayerState::Get().GetSurfaceWindow(pCreateInfo->surface);");
    const size_t adopt = swapchain.find("AdoptVulkanQueueDepthWindow(*sd);");
    ASSERT_NE(window, std::string::npos);
    ASSERT_NE(adopt, std::string::npos);
    EXPECT_LT(window, adopt) << "adoption is keyed by the swapchain's window";
    EXPECT_NE(pacing.find("it->second.device == swapchain.device"), std::string::npos)
        << "a reused window handle on another device must not inherit stale statistics";
    EXPECT_NE(pacing.find("\" at swapchain destroy\""), std::string::npos);
    EXPECT_NE(pacing.find("generations=%u"), std::string::npos);
}