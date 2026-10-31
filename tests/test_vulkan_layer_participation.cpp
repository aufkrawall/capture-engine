#include <gtest/gtest.h>

#include "common/platform/process_identity.h"
// The negotiation unit has no Vulkan/runtime dependency; compile its public
// interface here just as the lightweight gate does.
#include "hook/vulkan_layer/layer_participation.cpp"

namespace {
namespace participation = ce::vulkan_layer_participation;

struct PublishedParent {
    DiscoveryInfo discovery{};
    ce::selector_policy::Publisher publisher;
    ce::process::ProcessIdentityResult current = ce::process::QueryProcessIdentity(GetCurrentProcessId());

    bool Publish(const std::vector<std::string>& childSelectors) {
        DWORD parentPid = 0;
        if (!ce::process_parent::QueryParentProcessId(GetCurrentProcess(), &parentPid))
            return false;
        const auto parent = ce::process::QueryProcessIdentity(parentPid);
        if (!parent || !current)
            return false;
        discovery.SetInjectPid(GetCurrentProcessId());
        discovery.SetProfileTargetPid(parentPid);
        discovery.SetMagic(DISCOVERY_MAGIC);
        discovery.SetAbiSignature(SHARED_MEMORY_ABI_SIGNATURE);
        std::vector<std::string> selectors{parent.imagePath};
        selectors.insert(selectors.end(), childSelectors.begin(), childSelectors.end());
        return publisher.Publish(GetCurrentProcessId(), selectors, discovery.selectorPolicyGeneration);
    }
};

TEST(VulkanLayerParticipationTest, ExplicitChildDenialStopsParentInheritance) {
    PublishedParent host;
    ASSERT_TRUE(host.Publish({ce::config::EncodeProcessSelector(host.current.imagePath, false)}));
    const auto result = participation::EvaluateHostEligibility(&host.discovery, host.current.imageName.c_str());
    EXPECT_FALSE(result.eligible);
    EXPECT_TRUE(result.explicitlyDenied);
    EXPECT_EQ(result.inheritedParentPid, 0u);
    EXPECT_FALSE(ce::vulkan_layer_targets::ShouldLayerParticipate(true, result.eligible, true,
                                                                result.explicitlyDenied));
}

TEST(VulkanLayerParticipationTest, AnUnmatchedChildStillInheritsThePublishedParent) {
    PublishedParent host;
    ASSERT_TRUE(host.Publish({}));
    const auto result = participation::EvaluateHostEligibility(&host.discovery, host.current.imageName.c_str());
    EXPECT_TRUE(result.eligible);
    EXPECT_FALSE(result.explicitlyDenied);
    EXPECT_EQ(result.inheritedParentPid, host.discovery.GetProfileTargetPid());
}

TEST(VulkanLayerParticipationTest, MostSpecificChildDecisionStillWins) {
    PublishedParent host;
    ASSERT_TRUE(host.Publish({ce::config::EncodeProcessSelector(host.current.imageName, false),
                             host.current.imagePath}));
    const auto result = participation::EvaluateHostEligibility(&host.discovery, host.current.imageName.c_str());
    EXPECT_TRUE(result.eligible);
    EXPECT_FALSE(result.explicitlyDenied);
    EXPECT_EQ(result.inheritedParentPid, 0u);
}

}  // namespace
