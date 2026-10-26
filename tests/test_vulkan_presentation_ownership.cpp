#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hook/vulkan_layer/vulkan_presentation_ownership.h"

namespace {

struct Presentation {
    ce::vulkan_presentation::Ownership<uint64_t> ownership;
    std::vector<bool> publications;

    auto Publisher() {
        return [this](bool active) { publications.push_back(active); };
    }
};

TEST(VulkanPresentationOwnershipTest, AdapterEnumerationDoesNotClaimPresentation) {
    Presentation state;
    state.ownership.SetConnected(true, state.Publisher());
    // Connecting and creating instances/devices for adapter enumeration creates no swapchain.
    state.ownership.SetConnected(true, state.Publisher());
    state.ownership.CompleteCreation(0, 0, state.Publisher());
    EXPECT_TRUE(state.publications.empty());
    state.ownership.CompleteCreation(1, 0, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true}));
}

TEST(VulkanPresentationOwnershipTest, ReplacementHasNoOwnershipGapAndOldDestructionCannotClearIt) {
    Presentation state;
    state.ownership.SetConnected(true, state.Publisher());
    state.ownership.CompleteCreation(1, 0, state.Publisher());
    state.ownership.CompleteCreation(2, 1, state.Publisher());
    state.ownership.Destroyed(1, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true}));
    state.ownership.Destroyed(2, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true, false}));
}

TEST(VulkanPresentationOwnershipTest, OverlappingSwapchainsReleaseOnlyAfterTheLastOne) {
    Presentation state;
    state.ownership.SetConnected(true, state.Publisher());
    state.ownership.CompleteCreation(1, 0, state.Publisher());
    state.ownership.CompleteCreation(2, 0, state.Publisher());
    state.ownership.CompleteCreation(2, 0, state.Publisher());
    state.ownership.Destroyed(1, state.Publisher());
    state.ownership.Destroyed(99, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true}));
    state.ownership.Destroyed(2, state.Publisher());
    state.ownership.Destroyed(2, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true, false}));
}

TEST(VulkanPresentationOwnershipTest, ReconnectRepublishesExistingSwapchains) {
    Presentation state;
    state.ownership.CompleteCreation(1, 0, state.Publisher());
    EXPECT_TRUE(state.publications.empty());
    state.ownership.SetConnected(true, state.Publisher());
    state.ownership.SetConnected(false, state.Publisher());
    state.ownership.SetConnected(true, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true, false, true}));
}

TEST(VulkanPresentationOwnershipTest, DormantDestructionPreventsAStaleReconnectClaim) {
    Presentation state;
    state.ownership.SetConnected(true, state.Publisher());
    state.ownership.CompleteCreation(1, 0, state.Publisher());
    state.ownership.SetConnected(false, state.Publisher());
    state.ownership.Destroyed(1, state.Publisher());
    state.ownership.SetConnected(true, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true, false}));
}

TEST(VulkanPresentationOwnershipTest, ActualCreationProtectsDriverWsiAndFailureRollsBack) {
    Presentation state;
    state.ownership.SetConnected(true, state.Publisher());
    state.ownership.BeginCreation(state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true}));
    state.ownership.CompleteCreation(0, 0, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true, false}));
    state.ownership.BeginCreation(state.Publisher());
    state.ownership.CompleteCreation(1, 0, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true, false, true}));
    state.ownership.BeginCreation(state.Publisher());
    state.ownership.CompleteCreation(0, 1, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true, false, true}));
    state.ownership.Destroyed(1, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true, false, true, false}));
}

TEST(VulkanPresentationOwnershipTest, OverlappingCreationKeepsOwnershipUntilBothFinish) {
    Presentation state;
    state.ownership.SetConnected(true, state.Publisher());
    state.ownership.BeginCreation(state.Publisher());
    state.ownership.BeginCreation(state.Publisher());
    state.ownership.CompleteCreation(0, 0, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true}));
    state.ownership.CompleteCreation(2, 0, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true}));
    state.ownership.Destroyed(2, state.Publisher());
    EXPECT_EQ(state.publications, (std::vector<bool>{true, false}));
}

}  // namespace
