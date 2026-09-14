#include <gtest/gtest.h>

#include "../hook/common/ddraw_present_policy.h"

namespace policy = ce::ddraw_present_policy;

namespace {

policy::BlitGeometry FullScreenPresentBlit() {
    policy::BlitGeometry geometry;
    geometry.destIsScanout = true;
    geometry.destIsFlipChain = false;
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

TEST(DDrawPresentPolicyTest, BlitsOntoAFlipChainAreNotPresentations) {
    // Flip publishes a flip chain's images. Treating a draw into one as a
    // present composited the overlay into a buffer nothing was about to show.
    policy::BlitGeometry geometry = FullScreenPresentBlit();
    geometry.destIsFlipChain = true;
    EXPECT_EQ(policy::ClassifyBlit(geometry), policy::PresentKind::None);
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
