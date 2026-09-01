#include <gtest/gtest.h>

#include "../common/shared_defs.h"

TEST(CrossProcessGraphicsPublicationTest, VulkanOwnershipAndPresentEvidenceStayInsideThePublishingTree) {
    CaptureState state;
    constexpr uint32_t portalClientPid = 13356;
    constexpr uint32_t portalRendererPid = 18340;
    constexpr uint32_t filterTesterPid = 19920;
    constexpr uint32_t talosPid = 19156;

    EXPECT_EQ(state.PublishVulkanLayerClaim(portalRendererPid, portalClientPid), 0u);
    EXPECT_TRUE(state.IsVulkanLayerOwnedByProcess(portalRendererPid));
    EXPECT_TRUE(state.IsVulkanLayerOwnedByProcess(portalClientPid));
    EXPECT_FALSE(state.IsVulkanLayerOwnedByProcess(filterTesterPid));
    EXPECT_FALSE(state.IsVulkanLayerOwnedByProcess(talosPid));

    // The low 32-bit tick remains valid through GetTickCount's 49-day wrap.
    const uint64_t threadPublication =
        state.PublishVulkanPresent(portalRendererPid, 0x636Cu, 0xFFFFFFF0ull);
    ASSERT_NE(threadPublication, 0u);
    EXPECT_TRUE(state.IsVulkanPresentRecentForProcess(portalClientPid, 0x10ull, 32));
    EXPECT_FALSE(state.IsVulkanPresentRecentForProcess(filterTesterPid, 0x10ull, 32));
    EXPECT_EQ(state.GetVulkanPresentThreadForProcess(portalClientPid), 0x636Cu);
    EXPECT_EQ(state.GetVulkanPresentThreadForProcess(talosPid), 0u);

    state.ClearVulkanPresentThread(threadPublication);
    EXPECT_EQ(state.GetVulkanPresentThreadForProcess(portalClientPid), 0u);
}

TEST(CrossProcessGraphicsPublicationTest, OldVulkanPublisherCannotClearOrReactivateANewerTarget) {
    CaptureState state;
    constexpr uint32_t portalClientPid = 13356;
    constexpr uint32_t portalRendererPid = 18340;
    constexpr uint32_t filterTesterPid = 19920;

    state.PublishVulkanLayerClaim(portalRendererPid, portalClientPid);
    ASSERT_TRUE(state.SetVulkanOverlayActive(portalRendererPid, true));
    ASSERT_TRUE(state.IsVulkanOverlayActiveForProcess(portalClientPid));

    const uint64_t oldClaim = state.PublishVulkanLayerClaim(filterTesterPid, filterTesterPid);
    EXPECT_EQ(oldClaim, ce::vulkan_layer_claim::Make(portalRendererPid, portalClientPid));
    EXPECT_FALSE(state.IsVulkanOverlayActiveForProcess(filterTesterPid));
    EXPECT_FALSE(state.ReleaseVulkanLayerClaim(portalRendererPid));
    EXPECT_FALSE(state.SetVulkanOverlayActive(portalRendererPid, true));
    EXPECT_EQ(state.PublishVulkanPresent(portalRendererPid, 77, 1000), 0u);
    EXPECT_FALSE(state.IsVulkanPresentRecentForProcess(filterTesterPid, 1000, 2000));

    ASSERT_TRUE(state.SetVulkanOverlayActive(filterTesterPid, true));
    ASSERT_NE(state.PublishVulkanPresent(filterTesterPid, 88, 1000), 0u);
    EXPECT_TRUE(state.IsVulkanPresentRecentForProcess(filterTesterPid, 1000, 2000));
    EXPECT_TRUE(state.IsVulkanOverlayActiveForProcess(filterTesterPid));
    EXPECT_FALSE(state.SetVulkanOverlayActive(portalRendererPid, false));
    EXPECT_TRUE(state.IsVulkanOverlayActiveForProcess(filterTesterPid));
    EXPECT_TRUE(state.ReleaseVulkanLayerClaim(filterTesterPid));
    EXPECT_FALSE(state.IsVulkanLayerOwnedByProcess(filterTesterPid));
}

TEST(CrossProcessGraphicsPublicationTest, DlssFgPublicationIsAtomicAndScopedToTheExactRendererTree) {
    SharedMemoryLayout sharedMemory;
    constexpr uint32_t portalClientPid = 13356;
    constexpr uint32_t portalRendererPid = 18340;
    constexpr uint32_t filterTesterPid = 19920;
    constexpr uint32_t talosPid = 19156;
    const uint64_t portalClaim = ce::vulkan_layer_claim::Make(portalRendererPid, portalClientPid);

    sharedMemory.dlssState.PublishFGState(portalRendererPid, true, 3);
    auto portal = sharedMemory.dlssState.ReadFGStateForProcess(portalRendererPid, portalClaim);
    EXPECT_TRUE(portal.belongsToProcessTree);
    EXPECT_TRUE(portal.active);
    EXPECT_EQ(portal.multiplier, 3);

    // A split renderer may consume state published by its exact client.
    sharedMemory.dlssState.PublishFGState(portalClientPid, true, 4);
    portal = sharedMemory.dlssState.ReadFGStateForProcess(portalRendererPid, portalClaim);
    EXPECT_TRUE(portal.belongsToProcessTree);
    EXPECT_TRUE(portal.active);
    EXPECT_EQ(portal.multiplier, 4);

    // This is the supplied Portal -> Filter Tester -> Talos sequence: neither
    // unrelated process is allowed to inherit Portal's still-live publication.
    const uint64_t filterClaim = ce::vulkan_layer_claim::Make(filterTesterPid, filterTesterPid);
    const auto filter = sharedMemory.dlssState.ReadFGStateForProcess(filterTesterPid, filterClaim);
    const auto talos = sharedMemory.dlssState.ReadFGStateForProcess(talosPid, 0);
    EXPECT_EQ(filter.publisherPid, portalClientPid);
    EXPECT_FALSE(filter.belongsToProcessTree);
    EXPECT_FALSE(filter.active);
    EXPECT_EQ(filter.multiplier, 0);
    EXPECT_FALSE(talos.belongsToProcessTree);
    EXPECT_FALSE(talos.active);

    sharedMemory.dlssState.PublishFGState(filterTesterPid, false, 4);
    const auto filterOff = sharedMemory.dlssState.ReadFGStateForProcess(filterTesterPid, filterClaim);
    EXPECT_TRUE(filterOff.belongsToProcessTree);
    EXPECT_FALSE(filterOff.active);
    EXPECT_EQ(filterOff.multiplier, 0);
}
