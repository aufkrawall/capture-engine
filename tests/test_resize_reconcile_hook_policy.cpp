#include <gtest/gtest.h>

#include <cstring>

#include "hook/present/resize_reconcile_hook_policy.h"

namespace policy = ce::resize_reconcile_hook;

// Talos Reawakened logs/20260927_031545: with CE's jump at ResizeBuffers' entry the Steam
// overlay skipped its ResizeBuffers hook, kept one reference on each back buffer, and every
// resolution change was refused. With a third-party overlay loaded CE must never take the
// entry, even when its own body hook is refused; without one the entry stays the fallback.
TEST(ResizeReconcileHookPolicyTest, TheEntryIsNeverTakenWhileAThirdPartyOverlayIsLoaded) {
    EXPECT_EQ(policy::ChooseSiteAfterBodyHookRefused(true), policy::Site::kNone);
    EXPECT_EQ(policy::ChooseSiteAfterBodyHookRefused(false), policy::Site::kEntry);
}

TEST(ResizeReconcileHookPolicyTest, TheBodyHookClearsTheWidestForeignEntryPatch) {
    // FF 25 disp32 + 8-byte target: anything shallower could land inside a foreign patch.
    EXPECT_GE(policy::kAssumedForeignEntryPatchSize, 14);
    EXPECT_STREQ(policy::SiteName(policy::Site::kBodyBelowEntry), "body-below-entry");
    EXPECT_STREQ(policy::SiteName(policy::Site::kEntry), "entry");
    EXPECT_STREQ(policy::SiteName(policy::Site::kNone), "none");
}
