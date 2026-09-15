#include <gtest/gtest.h>

#include <iterator>

#include "../hook/common/ddraw_native_overlay_damage.h"
#include "../hook/common/ddraw_present_policy.h"

namespace policy = ce::ddraw_present_policy;
namespace native_damage = ce::ddraw_native_overlay;

namespace {

policy::BlitGeometry FullScreenPresentBlit() {
    policy::BlitGeometry geometry;
    geometry.destIsScanout = true;
    geometry.destIsBackBuffer = false;
    geometry.destOwnsFlipChain = false;
    geometry.haveSource = true;
    geometry.dest = {1920, 1080};
    geometry.source = {1920, 1080};
    return geometry;
}

}  // namespace

TEST(DDrawPresentPolicyTest, FlipChainCompositesIntoTheImageTheFlipPublishes) {
    // The regression this encodes: the overlay used to be written into the
    // surface that was already on screen after Flip returned, which races
    // scanout and is discarded by the next flip.
    EXPECT_EQ(policy::SelectCompositeTarget(policy::PresentKind::FlipChain, true),
              policy::CompositeTarget::PresentSource);
}

TEST(DDrawPresentPolicyTest, FlipWithoutAResolvedTargetCompositesNothing) {
    // Falling back to the visible surface here would reintroduce the race, so a
    // flip whose target could not be resolved draws no overlay at all.
    EXPECT_EQ(policy::SelectCompositeTarget(policy::PresentKind::FlipChain, false), policy::CompositeTarget::None);
}

TEST(DDrawPresentPolicyTest, BlitPresentCompositesIntoItsSource) {
    EXPECT_EQ(policy::SelectCompositeTarget(policy::PresentKind::BlitPresent, true),
              policy::CompositeTarget::PresentSource);
    EXPECT_EQ(policy::SelectCompositeTarget(policy::PresentKind::BlitPresent, false), policy::CompositeTarget::None);
}

TEST(DDrawPresentPolicyTest, DirectScanoutCompositesIntoTheVisibleSurface) {
    EXPECT_EQ(policy::SelectCompositeTarget(policy::PresentKind::DirectScanout, false),
              policy::CompositeTarget::VisibleSurface);
}

TEST(DDrawPresentPolicyTest, NonPresentationCallsCompositeNothing) {
    EXPECT_EQ(policy::SelectCompositeTarget(policy::PresentKind::None, true), policy::CompositeTarget::None);
}

TEST(DDrawPresentPolicyTest, FullSurfaceBlitOntoASingleBufferedPrimaryIsAPresent) {
    EXPECT_EQ(policy::ClassifyBlit(FullScreenPresentBlit()), policy::PresentKind::BlitPresent);
}

TEST(DDrawPresentPolicyTest, ExplicitFullRectanglesStillCountAsAPresent) {
    policy::BlitGeometry geometry = FullScreenPresentBlit();
    geometry.haveDestRect = true;
    geometry.destRect = {0, 0, 1920, 1080};
    geometry.haveSourceRect = true;
    geometry.sourceRect = {0, 0, 1920, 1080};
    EXPECT_EQ(policy::ClassifyBlit(geometry), policy::PresentKind::BlitPresent);
}

TEST(DDrawPresentPolicyTest, BlitsOntoABackBufferAreNotPresentations) {
    // Flip publishes a back buffer. Treating a draw into one as a present
    // composited the overlay into a buffer nothing was about to show.
    policy::BlitGeometry geometry = FullScreenPresentBlit();
    geometry.destIsScanout = false;
    geometry.destIsBackBuffer = true;
    geometry.destOwnsFlipChain = true;
    EXPECT_EQ(policy::ClassifyBlit(geometry), policy::PresentKind::None);
}

TEST(DDrawPresentPolicyTest, BlitsOntoAFlipChainsFrontBufferStillReachTheScreen) {
    // Gothic II draws its loading screens by blitting onto the primary while
    // the 3D scene is not running. Suppressing those because the primary heads
    // a flip chain left the overlay off the screen for the whole load: only a
    // back buffer is published later, the front buffer is the screen.
    policy::BlitGeometry geometry = FullScreenPresentBlit();
    geometry.destOwnsFlipChain = true;
    EXPECT_EQ(policy::ClassifyBlit(geometry), policy::PresentKind::DirectScanout);
    EXPECT_EQ(policy::SelectCompositeTarget(policy::ClassifyBlit(geometry), true),
              policy::CompositeTarget::VisibleSurface);
}

TEST(DDrawPresentPolicyTest, AFlipChainsBlitSourceIsNeverStamped) {
    // The source of such a blit can be a static image the application blits
    // again unchanged, so the overlay goes into the visible surface afterwards
    // instead of into the source.
    policy::BlitGeometry geometry = FullScreenPresentBlit();
    geometry.destOwnsFlipChain = true;
    EXPECT_NE(policy::SelectCompositeTarget(policy::ClassifyBlit(geometry), true),
              policy::CompositeTarget::PresentSource);
}

TEST(DDrawPresentPolicyTest, PartialBlitsAreScanoutUpdatesNotPresentations) {
    // A HUD piece or cursor blit is not a frame. Classifying it as one made the
    // old code re-composite the whole overlay dozens of times per frame.
    policy::BlitGeometry geometry = FullScreenPresentBlit();
    geometry.haveDestRect = true;
    geometry.destRect = {32, 32, 160, 96};
    EXPECT_EQ(policy::ClassifyBlit(geometry), policy::PresentKind::DirectScanout);
}

TEST(DDrawPresentPolicyTest, ScaledBlitsAreNotPresentations) {
    policy::BlitGeometry geometry = FullScreenPresentBlit();
    geometry.source = {960, 540};
    EXPECT_EQ(policy::ClassifyBlit(geometry), policy::PresentKind::DirectScanout);
}

TEST(DDrawPresentPolicyTest, BlitsOntoOffscreenSurfacesAreIgnored) {
    policy::BlitGeometry geometry = FullScreenPresentBlit();
    geometry.destIsScanout = false;
    EXPECT_EQ(policy::ClassifyBlit(geometry), policy::PresentKind::None);
}

TEST(DDrawPresentPolicyTest, ScanoutClearWithoutASourceIsADirectUpdate) {
    policy::BlitGeometry geometry = FullScreenPresentBlit();
    geometry.haveSource = false;
    EXPECT_EQ(policy::ClassifyBlit(geometry), policy::PresentKind::DirectScanout);
}

TEST(DDrawPresentPolicyTest, DirectScanoutSkipsUpdatesThatMissTheOverlay) {
    const policy::Rect overlayBounds{15, 15, 480, 260};
    EXPECT_FALSE(policy::DirectScanoutNeedsComposite(overlayBounds, true, policy::Rect{900, 900, 1000, 1000}));
    EXPECT_TRUE(policy::DirectScanoutNeedsComposite(overlayBounds, true, policy::Rect{400, 200, 1000, 1000}));
    // An update with no rectangle covers the surface and therefore the overlay.
    EXPECT_TRUE(policy::DirectScanoutNeedsComposite(overlayBounds, false, policy::Rect{}));
    // Nothing drawn means nothing to restore.
    EXPECT_FALSE(policy::DirectScanoutNeedsComposite(policy::Rect{}, false, policy::Rect{}));
}

TEST(DDrawPresentPolicyTest, DirectScanoutTreatsTouchingEdgesAsDisjoint) {
    const policy::Rect overlayBounds{0, 0, 100, 100};
    EXPECT_FALSE(policy::DirectScanoutNeedsComposite(overlayBounds, true, policy::Rect{100, 0, 200, 100}));
    EXPECT_TRUE(policy::DirectScanoutNeedsComposite(overlayBounds, true, policy::Rect{99, 0, 200, 100}));
}

TEST(DDrawPresentPolicyTest, SingleBufferedScanoutWritesAreAlwaysPresentations) {
    EXPECT_TRUE(policy::ScanoutWriteIsPresentation(/*destOwnsFlipChain=*/false, 0));
    EXPECT_TRUE(policy::ScanoutWriteIsPresentation(false, 1));
}

TEST(DDrawPresentPolicyTest, AFrontBufferWriteNeedsTwoWritesWithoutAFlip) {
    EXPECT_EQ(policy::kScanoutWritesWithoutFlipThreshold, 2u);
    EXPECT_FALSE(policy::ScanoutWriteIsPresentation(/*destOwnsFlipChain=*/true, 1));
    EXPECT_TRUE(policy::ScanoutWriteIsPresentation(true, 2));
    EXPECT_TRUE(policy::ScanoutWriteIsPresentation(true, 42));
}

TEST(DDrawPresentPolicyTest, CompositeRegionGrowsToTheStagingGrid) {
    policy::Rect region;
    ASSERT_TRUE(policy::AlignCompositeRegion(policy::Rect{14, 14, 470, 250}, 3840, 2160, 64, region));
    EXPECT_EQ(region.left, 0);
    EXPECT_EQ(region.top, 0);
    EXPECT_EQ(region.right, 512);
    EXPECT_EQ(region.bottom, 256);
}

TEST(DDrawPresentPolicyTest, CompositeRegionStaysInsideTheSurface) {
    // A stale viewport must never produce a lock rectangle past the surface.
    policy::Rect region;
    ASSERT_TRUE(policy::AlignCompositeRegion(policy::Rect{600, 400, 799, 599}, 800, 600, 64, region));
    EXPECT_EQ(region.right, 800);
    EXPECT_EQ(region.bottom, 600);
    EXPECT_EQ(region.left, 576);
    EXPECT_EQ(region.top, 384);
}

TEST(DDrawPresentPolicyTest, CompositeRegionRejectsEmptyAndDegenerateInput) {
    policy::Rect region;
    EXPECT_FALSE(policy::AlignCompositeRegion(policy::Rect{10, 10, 10, 40}, 800, 600, 64, region));
    EXPECT_FALSE(policy::AlignCompositeRegion(policy::Rect{10, 10, 40, 40}, 0, 600, 64, region));
    EXPECT_FALSE(policy::AlignCompositeRegion(policy::Rect{900, 10, 950, 40}, 800, 600, 64, region));
}

TEST(DDrawPresentPolicyTest, CompositeRegionToleratesADegenerateAlignment) {
    policy::Rect region;
    ASSERT_TRUE(policy::AlignCompositeRegion(policy::Rect{10, 10, 40, 40}, 800, 600, 0, region));
    EXPECT_EQ(region.left, 10);
    EXPECT_EQ(region.right, 40);
}

TEST(DDrawPresentPolicyTest, RegionSizeIsAFractionOfTheFrameItReplaces) {
    // The point of the region: a 4K overlay composite must not move the frame.
    policy::Rect region;
    ASSERT_TRUE(policy::AlignCompositeRegion(policy::Rect{15, 15, 700, 400}, 3840, 2160, 64, region));
    const long long regionPixels =
        static_cast<long long>(region.right - region.left) * (region.bottom - region.top);
    const long long framePixels = 3840LL * 2160LL;
    EXPECT_LT(regionPixels * 20, framePixels);
}

TEST(DDrawPresentPolicyTest, NativeEndSceneRenderingHonorsOverlayExcludedRecording) {
    EXPECT_TRUE(policy::NativeOverlayShouldDraw(true, true, /*recording=*/false,
                                                /*captureIncludesOverlay=*/false));
    EXPECT_TRUE(policy::NativeOverlayShouldDraw(true, true, true, true));
    EXPECT_FALSE(policy::NativeOverlayShouldDraw(true, true, true, false));
    EXPECT_FALSE(policy::NativeOverlayShouldDraw(false, true, false, true));
    EXPECT_FALSE(policy::NativeOverlayShouldDraw(true, false, false, true));
}

TEST(DDrawPresentPolicyTest, TheWrittenRectangleCoversWhatCeWroteLastTime) {
    // A shrinking overlay would otherwise leave the strip it vacated holding
    // the previous composite, with nothing left to repaint it.
    const policy::Rect now{0, 0, 448, 320};
    const policy::Rect previous{0, 0, 512, 384};
    const policy::Rect merged = policy::ExpandToPreviousComposite(now, true, previous);
    EXPECT_EQ(merged.right, 512);
    EXPECT_EQ(merged.bottom, 384);
}

TEST(DDrawPresentPolicyTest, AGrowingOverlayKeepsItsOwnRectangle) {
    const policy::Rect now{0, 0, 512, 384};
    const policy::Rect previous{0, 0, 448, 320};
    const policy::Rect merged = policy::ExpandToPreviousComposite(now, true, previous);
    EXPECT_EQ(merged.right, 512);
    EXPECT_EQ(merged.bottom, 384);
}

TEST(DDrawPresentPolicyTest, NoPreviousCompositeLeavesTheRectangleAlone) {
    const policy::Rect now{64, 64, 512, 384};
    const policy::Rect merged = policy::ExpandToPreviousComposite(now, false, policy::Rect{});
    EXPECT_EQ(merged.left, 64);
    EXPECT_EQ(merged.top, 64);
    EXPECT_EQ(merged.right, 512);
    EXPECT_EQ(merged.bottom, 384);
}

TEST(DDrawPresentPolicyTest, ARealHigherLevelDeviceAlwaysSuppressesSyntheticDirectDrawBootstrap) {
    EXPECT_TRUE(policy::ShouldSkipDirectDrawBootstrap(/*higherLevelDeviceCreated=*/true,
                                                      /*higherLevelModuleLoaded=*/false,
                                                      /*directDrawEvidence=*/true));
}

TEST(DDrawPresentPolicyTest, ALoadedD3DModuleAloneDoesNotOverrideObservedDirectDraw) {
    EXPECT_FALSE(policy::ShouldSkipDirectDrawBootstrap(/*higherLevelDeviceCreated=*/false,
                                                       /*higherLevelModuleLoaded=*/true,
                                                       /*directDrawEvidence=*/true));
}

TEST(DDrawPresentPolicyTest, ALoadedD3DModuleDefersBootstrapUntilTheApplicationProvidesEvidence) {
    EXPECT_TRUE(policy::ShouldSkipDirectDrawBootstrap(/*higherLevelDeviceCreated=*/false,
                                                      /*higherLevelModuleLoaded=*/true,
                                                      /*directDrawEvidence=*/false));
    EXPECT_FALSE(policy::ShouldSkipDirectDrawBootstrap(/*higherLevelDeviceCreated=*/false,
                                                       /*higherLevelModuleLoaded=*/false,
                                                       /*directDrawEvidence=*/false));
}

TEST(DDrawPresentPolicyTest, StoredRgb565CompositeRoundTripsToAStableCanonicalValue) {
    const uint32_t blended = policy::BlendPremultipliedOver(0x80613527u, 0xFF173B91u);
    const uint16_t stored = policy::PackRgb565(blended);
    const uint32_t canonical = policy::ExpandRgb565(stored);
    EXPECT_EQ(policy::PackRgb565(canonical), stored);
    EXPECT_EQ(policy::ExpandRgb565(policy::PackRgb565(canonical)), canonical);
}

TEST(DDrawPresentPolicyTest, StoredRgb555CompositeRoundTripsToAStableCanonicalValue) {
    const uint32_t blended = policy::BlendPremultipliedOver(0x80613527u, 0xFF173B91u);
    const uint16_t stored = policy::PackRgb555(blended);
    const uint32_t canonical = policy::ExpandRgb555(stored);
    EXPECT_EQ(policy::PackRgb555(canonical), stored);
    EXPECT_EQ(policy::ExpandRgb555(policy::PackRgb555(canonical)), canonical);
}

TEST(DDrawPresentPolicyTest, NativeOverlayIgnoresExactWritesOutsideItsBounds) {
    native_damage::DamageTracker tracker;
    tracker.SetCurrent({10, 10, 110, 110});
    tracker.RecordWrite(true, {200, 200, 220, 220});
    size_t count = 99;
    EXPECT_EQ(tracker.CopyRepairs(nullptr, 0, count), native_damage::State::Current);
    EXPECT_EQ(count, 0u);
}

TEST(DDrawPresentPolicyTest, NativeOverlayRetainsSeparateLShapedRepairRectangles) {
    native_damage::DamageTracker tracker;
    tracker.SetCurrent({0, 0, 100, 100});
    tracker.RecordWrite(true, {0, 0, 60, 20});
    tracker.RecordWrite(true, {0, 0, 20, 60});
    policy::Rect repairs[native_damage::DamageTracker::kMaxRepairRects] = {};
    size_t count = 0;
    EXPECT_EQ(tracker.CopyRepairs(repairs, std::size(repairs), count), native_damage::State::Repairable);
    EXPECT_EQ(count, 2u);
}

TEST(DDrawPresentPolicyTest, NativeOverlayCoalescesGaplessRepairsAndBecomesCurrentAgain) {
    native_damage::DamageTracker tracker;
    tracker.SetCurrent({0, 0, 100, 100});
    tracker.RecordWrite(true, {10, 10, 30, 30});
    tracker.RecordWrite(true, {30, 10, 50, 30});
    policy::Rect repairs[native_damage::DamageTracker::kMaxRepairRects] = {};
    size_t count = 0;
    ASSERT_EQ(tracker.CopyRepairs(repairs, std::size(repairs), count), native_damage::State::Repairable);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(repairs[0].left, 10);
    EXPECT_EQ(repairs[0].right, 50);
    tracker.CompleteRepair(repairs[0]);
    EXPECT_EQ(tracker.CopyRepairs(nullptr, 0, count), native_damage::State::Current);
}

TEST(DDrawPresentPolicyTest, FullExactWriteRemovesAllOldNativeOverlayPixels) {
    native_damage::DamageTracker tracker;
    tracker.SetCurrent({10, 10, 110, 110});
    tracker.RecordWrite(true, {0, 0, 200, 200});
    size_t count = 0;
    EXPECT_EQ(tracker.CopyRepairs(nullptr, 0, count), native_damage::State::Absent);
}

TEST(DDrawPresentPolicyTest, UnknownOrOverflowedNativeDamageRefusesUnsafeFullReblend) {
    native_damage::DamageTracker unknown;
    unknown.SetCurrent({0, 0, 100, 100});
    unknown.RecordWrite(false, {});
    size_t count = 0;
    EXPECT_EQ(unknown.CopyRepairs(nullptr, 0, count), native_damage::State::Unsafe);

    native_damage::DamageTracker overflowed;
    overflowed.SetCurrent({0, 0, 100, 100});
    for (int i = 0; i <= static_cast<int>(native_damage::DamageTracker::kMaxRepairRects); ++i)
        overflowed.RecordWrite(true, {i * 3, 0, i * 3 + 1, 1});
    EXPECT_EQ(overflowed.CopyRepairs(nullptr, 0, count), native_damage::State::Unsafe);
}
