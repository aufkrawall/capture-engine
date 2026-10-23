#include <gtest/gtest.h>

#include "hook/streamline/streamline_foreign_entry_policy.h"

namespace {

using ce::streamline_runtime_policy::ResolveStreamlineHookSlotRequest;
using ce::streamline_runtime_policy::ShouldLeaveForeignOwnedStreamlineEntry;
using ce::streamline_runtime_policy::StreamlineHookSlotRequest;

TEST(StreamlineForeignEntryPolicy, RoutedExportsLeaveAForeignEntryOutsideTheTakeover) {
    EXPECT_TRUE(ShouldLeaveForeignOwnedStreamlineEntry(true, false));
}

TEST(StreamlineForeignEntryPolicy, TakeoverOwnedImportSlotsKeepTheEntryChain) {
    // The takeover's gates bind the game's static imports to the raw exports; the entry hook is
    // then CE's only view of those calls.
    EXPECT_FALSE(ShouldLeaveForeignOwnedStreamlineEntry(true, true));
}

TEST(StreamlineForeignEntryPolicy, ExportsWithoutACallerSideRouteKeepTheEntryChain) {
    // vkCreateSwapchainKHR and the 1.x payload recorders have no GetProcAddress-time route.
    EXPECT_FALSE(ShouldLeaveForeignOwnedStreamlineEntry(false, false));
    EXPECT_FALSE(ShouldLeaveForeignOwnedStreamlineEntry(false, true));
}

TEST(StreamlineForeignEntryPolicy, AnEmptySlotInstallsOnTheRequestedExport) {
    int exportA = 0;
    EXPECT_EQ(ResolveStreamlineHookSlotRequest(nullptr, &exportA, false), StreamlineHookSlotRequest::kInstall);
}

TEST(StreamlineForeignEntryPolicy, ALeftExportIsNotReEvaluatedOnEveryModuleInspection) {
    // sl.common is inspected several times per launch; each pass would otherwise read the image from
    // disk again for an entry that is still the other component's.
    int exportA = 0;
    EXPECT_EQ(ResolveStreamlineHookSlotRequest(&exportA, &exportA, true), StreamlineHookSlotRequest::kAlreadyHandled);
}

TEST(StreamlineForeignEntryPolicy, ALeftExportStillOwnsTheSingleForwardPointer) {
    // A second live instance of the same export must not take over the slot's one original, whether
    // CE hooked the first instance or left it to its owner.
    int exportA = 0;
    int exportB = 0;
    EXPECT_EQ(ResolveStreamlineHookSlotRequest(&exportA, &exportB, true),
              StreamlineHookSlotRequest::kRefuseSecondInstance);
}

TEST(StreamlineForeignEntryPolicy, ARecordedExportWhoseModuleIsGoneIsAReloadGeneration) {
    int exportA = 0;
    int exportB = 0;
    EXPECT_EQ(ResolveStreamlineHookSlotRequest(&exportA, &exportB, false), StreamlineHookSlotRequest::kInstall);
}

}  // namespace
