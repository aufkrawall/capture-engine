#include <gtest/gtest.h>

#include "hook/d3d12/dx12_factory_bootstrap.h"

namespace {

struct BootstrapEnvironment {
    bool inLoaderCallout = false;
    bool vulkanActive = false;
    bool residentVulkanLayer = false;
    bool dxvkEvidence = false;
    unsigned loaderDeferrals = 0;
    unsigned vulkanBypasses = 0;
    unsigned layerQueries = 0;

    bool IsInLoaderCallout() const { return inLoaderCallout; }
    void NoteLoaderDeferral() { ++loaderDeferrals; }
    bool IsVulkanActive() const { return vulkanActive; }
    bool HasResidentVulkanLayer() {
        ++layerQueries;
        return residentVulkanLayer;
    }
    bool HasDXVKEvidence() const { return dxvkEvidence; }
    void NoteVulkanBypass() { ++vulkanBypasses; }
};

TEST(Dx12FactoryBootstrapTest, NestedLoaderReturnDefersWithoutEnteringInstaller) {
    BootstrapEnvironment environment;
    environment.inLoaderCallout = true;
    unsigned installs = 0;
    if (ce::dx12_factory_bootstrap::CanInstall(environment)) {
        ++installs;
    }
    EXPECT_EQ(installs, 0u);
    EXPECT_EQ(environment.loaderDeferrals, 1u);
    EXPECT_EQ(environment.layerQueries, 0u);

    // The hook-thread retry is admitted as soon as the loader callout has finished;
    // there is no permanent latch or timing assumption.
    environment.inLoaderCallout = false;
    if (ce::dx12_factory_bootstrap::CanInstall(environment)) {
        ++installs;
    }
    EXPECT_EQ(installs, 1u);
    EXPECT_EQ(environment.loaderDeferrals, 1u);
}

TEST(Dx12FactoryBootstrapTest, CompletedOrdinaryLoadInstallsSynchronously) {
    BootstrapEnvironment environment;
    EXPECT_TRUE(ce::dx12_factory_bootstrap::CanInstall(environment));
    EXPECT_EQ(environment.loaderDeferrals, 0u);
    EXPECT_EQ(environment.vulkanBypasses, 0u);
}

TEST(Dx12FactoryBootstrapTest, ResidentLayerPreventsDxgiMutationBeforeOwnershipPublication) {
    BootstrapEnvironment environment;
    environment.residentVulkanLayer = true;
    ASSERT_FALSE(environment.vulkanActive);
    EXPECT_FALSE(ce::dx12_factory_bootstrap::CanInstall(environment));
    EXPECT_EQ(environment.vulkanBypasses, 1u);
    EXPECT_EQ(environment.loaderDeferrals, 0u);

    environment.vulkanActive = true;
    EXPECT_FALSE(ce::dx12_factory_bootstrap::CanInstall(environment));
    EXPECT_EQ(environment.vulkanBypasses, 2u);
}

TEST(Dx12FactoryBootstrapTest, PublishedVulkanOwnershipBypassesEvenWithoutResidentLayer) {
    BootstrapEnvironment environment;
    environment.vulkanActive = true;
    EXPECT_FALSE(ce::dx12_factory_bootstrap::CanInstall(environment));
    EXPECT_EQ(environment.layerQueries, 0u);
    EXPECT_EQ(environment.vulkanBypasses, 1u);

    // A renderer change may re-enable D3D. Bypassing Vulkan never latches installation complete.
    environment.vulkanActive = false;
    EXPECT_TRUE(ce::dx12_factory_bootstrap::CanInstall(environment));
    EXPECT_EQ(environment.vulkanBypasses, 1u);
}

TEST(Dx12FactoryBootstrapTest, DxvkEvidencePreventsDxgiMutationBeforeLayerLoad) {
    BootstrapEnvironment environment;
    environment.dxvkEvidence = true;
    ASSERT_FALSE(environment.vulkanActive);
    ASSERT_FALSE(environment.residentVulkanLayer);
    EXPECT_FALSE(ce::dx12_factory_bootstrap::CanInstall(environment));
    EXPECT_EQ(environment.vulkanBypasses, 1u);
    EXPECT_EQ(environment.loaderDeferrals, 0u);
}

}  // namespace
