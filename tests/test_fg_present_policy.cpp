#include <gtest/gtest.h>
#include "../testapp/fg_present_policy.h"

using testapp::fg::ProxyPresentPolicy;
using testapp::fg::ResolveProxyPresentPolicy;

// Regression for the GPU device-hung after suspending DLSS FG under vsync: the Streamline proxy
// swapchain must be presented uncapped for the WHOLE DLSS mode -- active AND suspended (suspend
// keeps the proxy and its pacer; presenting it with SyncInterval=1 stalled the pacer on the
// suspend fence and hung the GPU device, 0x887a0005/6).
TEST(ProxyPresentPolicyTest, DlssModeIsAlwaysUncappedEvenWhenVsyncConfigured) {
    const ProxyPresentPolicy policy = ResolveProxyPresentPolicy(true, 1, true);
    EXPECT_EQ(policy.syncInterval, 0u);
}

TEST(ProxyPresentPolicyTest, NonDlssModesHonorConfiguredVsync) {
    const ProxyPresentPolicy vsyncOn = ResolveProxyPresentPolicy(false, 1, true);
    EXPECT_EQ(vsyncOn.syncInterval, 1u);
    EXPECT_FALSE(vsyncOn.allowTearing);
    const ProxyPresentPolicy vsyncOff = ResolveProxyPresentPolicy(false, 0, true);
    EXPECT_EQ(vsyncOff.syncInterval, 0u);
}

TEST(ProxyPresentPolicyTest, TearingFollowsUserVsyncIntentNotForcedSyncInterval) {
    // DLSS mode forces sync=0; with vsync configured on it must NOT tear.
    const ProxyPresentPolicy forced = ResolveProxyPresentPolicy(true, 1, true);
    EXPECT_EQ(forced.syncInterval, 0u);
    EXPECT_FALSE(forced.allowTearing);
    // User-requested uncapped presents tear when the swapchain supports it.
    const ProxyPresentPolicy uncapped = ResolveProxyPresentPolicy(false, 0, true);
    EXPECT_TRUE(uncapped.allowTearing);
    const ProxyPresentPolicy noTearingSupport = ResolveProxyPresentPolicy(false, 0, false);
    EXPECT_FALSE(noTearingSupport.allowTearing);
    const ProxyPresentPolicy dlssUncapped = ResolveProxyPresentPolicy(true, 0, true);
    EXPECT_TRUE(dlssUncapped.allowTearing);
}
