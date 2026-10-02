#include <gtest/gtest.h>

#include "hook/vulkan_layer/vulkan_capture_transport_policy.h"

namespace {

using ce::vulkan_capture_transport::CanServeUnadoptedTransport;
using ce::vulkan_capture_transport::DecideHostRepublish;
using ce::vulkan_capture_transport::HostRepublish;
using ce::vulkan_capture_transport::HostRepublishInput;
using ce::vulkan_capture_transport::PublishesRelayFence;
using ce::vulkan_capture_transport::RebuildInvalidatesCurrentState;

HostRepublishInput CurrentRelayTransport() {
    HostRepublishInput input;
    input.locksHeld = true;
    input.stateCurrent = true;
    input.entryFound = true;
    input.entryHandlesComplete = true;
    return input;
}

TEST(VulkanCaptureTransportPolicyTest, RepublishesALayerOwnedTransport) {
    const HostRepublishInput input = CurrentRelayTransport();
    EXPECT_EQ(DecideHostRepublish(input), HostRepublish::Published);
    EXPECT_FALSE(RebuildInvalidatesCurrentState(input));
}

// DXVK: after the layer adopted media's encoder textures, the cached entry is
// an import whose handles are all null (media already owned the textures). A
// replacement host used to receive those null handles plus the relay fence,
// which stopped being signaled at adoption.
TEST(VulkanCaptureTransportPolicyTest, AdoptedEncoderTextureImportIsRebuiltForANewHost) {
    HostRepublishInput input = CurrentRelayTransport();
    input.entryIsEncoderTextureImport = true;
    input.entryHandlesComplete = false;
    EXPECT_EQ(DecideHostRepublish(input), HostRepublish::Rebuild);
    EXPECT_TRUE(RebuildInvalidatesCurrentState(input));

    // Even an import that somehow carried handles is not the new host's.
    input.entryHandlesComplete = true;
    EXPECT_EQ(DecideHostRepublish(input), HostRepublish::Rebuild);
    EXPECT_TRUE(RebuildInvalidatesCurrentState(input));
}

// Capture initialization returns early for an unchanged initialized state, so
// a rebuild that left the state initialized published nothing for the host.
TEST(VulkanCaptureTransportPolicyTest, RebuildOfACurrentStateInvalidatesIt) {
    HostRepublishInput input = CurrentRelayTransport();
    input.entryFound = false;
    input.entryHandlesComplete = false;
    EXPECT_EQ(DecideHostRepublish(input), HostRepublish::Rebuild);
    EXPECT_TRUE(RebuildInvalidatesCurrentState(input));

    input = CurrentRelayTransport();
    input.entryHandlesComplete = false;
    EXPECT_EQ(DecideHostRepublish(input), HostRepublish::Rebuild);
    EXPECT_TRUE(RebuildInvalidatesCurrentState(input));
}

TEST(VulkanCaptureTransportPolicyTest, WithoutACurrentStateInitializationBuildsOneUntouched) {
    HostRepublishInput input;
    input.locksHeld = true;
    EXPECT_EQ(DecideHostRepublish(input), HostRepublish::Rebuild);
    EXPECT_FALSE(RebuildInvalidatesCurrentState(input));
}

// A contended lock used to fall through to initialization, which found the
// state unchanged and returned; the host generation was then recorded as
// served and never retried.
TEST(VulkanCaptureTransportPolicyTest, ContendedLocksRetryInsteadOfRecordingTheHost) {
    HostRepublishInput input = CurrentRelayTransport();
    input.locksHeld = false;
    EXPECT_EQ(DecideHostRepublish(input), HostRepublish::Retry);
    EXPECT_FALSE(RebuildInvalidatesCurrentState(input));

    input.entryIsEncoderTextureImport = true;
    EXPECT_EQ(DecideHostRepublish(input), HostRepublish::Retry);
    EXPECT_FALSE(RebuildInvalidatesCurrentState(input));
}

// A same-size swapchain rebuild after the recording that adopted media's
// textures must not reuse that import as an unadopted transport.
TEST(VulkanCaptureTransportPolicyTest, OnlyLayerOwnedEntriesServeAnUnadoptedTransport) {
    EXPECT_TRUE(CanServeUnadoptedTransport(true, false));
    EXPECT_FALSE(CanServeUnadoptedTransport(true, true));
    EXPECT_FALSE(CanServeUnadoptedTransport(false, false));
    EXPECT_FALSE(CanServeUnadoptedTransport(false, true));
}

TEST(VulkanCaptureTransportPolicyTest, OnlyTheRelaySignalsTheRelayFence) {
    EXPECT_TRUE(PublishesRelayFence(true, true));
    EXPECT_FALSE(PublishesRelayFence(true, false));
    // After adoption the state still holds the relay fence it no longer signals.
    EXPECT_FALSE(PublishesRelayFence(false, true));
    EXPECT_FALSE(PublishesRelayFence(false, false));
}

}  // namespace
