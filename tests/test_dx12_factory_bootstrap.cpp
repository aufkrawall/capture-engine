#include <gtest/gtest.h>

#include "hook/d3d12/dx12_factory_bootstrap.h"

namespace {

struct BootstrapEnvironment {
    bool inLoaderCallout = false;
    unsigned loaderDeferrals = 0;

    bool IsInLoaderCallout() const { return inLoaderCallout; }
    void NoteLoaderDeferral() { ++loaderDeferrals; }
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
}

}  // namespace
