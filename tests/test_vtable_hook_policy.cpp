#include <gtest/gtest.h>

#include "../hook/wrappers/vtable_hook_policy.h"

namespace policy = ce::vtable_hook_policy;

// Gothic II 20260916_011148: CE and Steam's gameoverlayrenderer each hooked
// IDirectDrawSurface7's Flip slot, and in that install order each one's saved
// original was the other's detour - three frames repeating down eight megabytes
// of the render thread's stack. Nothing CE does at install time can stop the
// other injector reinstalling afterwards, but CE can see that the pointer it is
// keeping as "the original" is not the vtable's own code, which is the one fact
// the resulting recursion cannot report about itself.
TEST(VTableHookPolicyTest, ChainingIntoAnotherInjectorsEntryIsReportedAtInstallTime) {
    int ddrawStorage = 0;
    int overlayStorage = 0;
    int selfStorage = 0;
    const void* vtableModule = &ddrawStorage;
    const void* overlayModule = &overlayStorage;
    const void* selfModule = &selfStorage;

    EXPECT_TRUE(policy::SavedOriginalIsForeignChain(overlayModule, vtableModule, selfModule));

    // The vtable's own implementation is not a chain.
    EXPECT_FALSE(policy::SavedOriginalIsForeignChain(vtableModule, vtableModule, selfModule));
    // CE's own detour is refused earlier, by the self-hook check.
    EXPECT_FALSE(policy::SavedOriginalIsForeignChain(selfModule, vtableModule, selfModule));
    // A vtable outside any module image has no owner to compare against, so
    // nothing is claimed for it.
    EXPECT_FALSE(policy::SavedOriginalIsForeignChain(overlayModule, nullptr, selfModule));
    EXPECT_FALSE(policy::SavedOriginalIsForeignChain(nullptr, vtableModule, selfModule));
}

// The ownership rules the repair path already had, kept next to the new one.
TEST(VTableHookPolicyTest, RepairReclaimsOnlyAConfirmedPredecessorRestoration) {
    int predecessorStorage = 0;
    int detourStorage = 0;
    int foreignStorage = 0;
    const void* predecessor = &predecessorStorage;
    const void* detour = &detourStorage;
    const void* foreign = &foreignStorage;

    EXPECT_TRUE(policy::ShouldReclaimRestoredSlot(predecessor, detour, predecessor));
    EXPECT_FALSE(policy::ShouldReclaimRestoredSlot(foreign, detour, predecessor));
    EXPECT_TRUE(policy::ShouldPreserveForeignFollower(foreign, detour, predecessor));
}
