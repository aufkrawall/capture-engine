#include <gtest/gtest.h>

#include "../hook/common/ddraw_present_policy.h"

namespace policy = ce::ddraw_present_policy;

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

// ============================================================================
// Overlay route - which renderer may draw a given presentation, and the rule
// that nothing renders while the loaded backend cannot serve the route.
// ============================================================================

TEST(DDrawPresentPolicyTest, OnlyAFlipTheDeviceRenderedCanUseTheNativeRoute) {
    EXPECT_EQ(policy::SelectOverlayRoute(policy::PresentKind::FlipChain, true), policy::OverlayRoute::NativeDevice);
    // The device is rendering somewhere else, so drawing with it would put the
    // overlay where nobody looks.
    EXPECT_EQ(policy::SelectOverlayRoute(policy::PresentKind::FlipChain, false),
              policy::OverlayRoute::HelperComposite);
}

TEST(DDrawPresentPolicyTest, BlitAndDirectScanoutPresentationsAlwaysComposite) {
    // A blit publishes an offscreen image and a direct scanout write is not a
    // device operation at all, so the application's 3D device cannot draw them
    // even when it is otherwise live.
    EXPECT_EQ(policy::SelectOverlayRoute(policy::PresentKind::BlitPresent, true),
              policy::OverlayRoute::HelperComposite);
    EXPECT_EQ(policy::SelectOverlayRoute(policy::PresentKind::DirectScanout, true),
              policy::OverlayRoute::HelperComposite);
    EXPECT_EQ(policy::SelectOverlayRoute(policy::PresentKind::None, true), policy::OverlayRoute::HelperComposite);
}

TEST(DDrawPresentPolicyTest, TheCompositeRouteRefusesToRenderThroughTheNativeBackend) {
    // The Gothic II crash, encoded: the composite ran while the backend was
    // still bound to the application's own Direct3D 7 device, so every frame
    // issued device work the application never asked for and then read back a
    // helper backbuffer the overlay had never been drawn into.
    EXPECT_FALSE(policy::BackendCanRenderRoute(policy::OverlayRoute::HelperComposite,
                                               /*nativeBackendBoundToThisDevice=*/true,
                                               /*compositeBackendReady=*/false));
    EXPECT_TRUE(policy::BackendCanRenderRoute(policy::OverlayRoute::HelperComposite, false, true));
}

TEST(DDrawPresentPolicyTest, TheNativeRouteRefusesToRenderThroughTheCompositeBackend) {
    EXPECT_FALSE(policy::BackendCanRenderRoute(policy::OverlayRoute::NativeDevice,
                                               /*nativeBackendBoundToThisDevice=*/false,
                                               /*compositeBackendReady=*/true));
    EXPECT_TRUE(policy::BackendCanRenderRoute(policy::OverlayRoute::NativeDevice, true, false));
}

TEST(DDrawPresentPolicyTest, ANativeBackendBoundToAnotherDeviceCannotRender) {
    // A device the application recreated leaves the backend bound to one that
    // renders nothing; the caller reports that as "not bound to this device".
    EXPECT_FALSE(policy::BackendCanRenderRoute(policy::OverlayRoute::NativeDevice, false, false));
}

TEST(DDrawPresentPolicyTest, NoBackendLoadedRendersNothingOnEitherRoute) {
    EXPECT_FALSE(policy::BackendCanRenderRoute(policy::OverlayRoute::NativeDevice, false, false));
    EXPECT_FALSE(policy::BackendCanRenderRoute(policy::OverlayRoute::HelperComposite, false, false));
}

// ============================================================================
// Scanout writes on a flip chain - drawing the next flip replaces, or the
// presentation itself once the chain has stopped flipping.
// ============================================================================

TEST(DDrawPresentPolicyTest, ASingleBufferedScanoutWriteIsAlwaysThePresentation) {
    // Nothing else publishes that surface, so every write to it is the frame.
    EXPECT_TRUE(policy::ScanoutWriteIsPresentation(/*destOwnsFlipChain=*/false, 1));
    EXPECT_TRUE(policy::ScanoutWriteIsPresentation(false, 0));
}

TEST(DDrawPresentPolicyTest, TheFirstFrontBufferWriteAfterAFlipIsDrawing) {
    // Gothic II writes its front buffer about eleven times a second while
    // flipping eighty-six times a second. Compositing on those writes puts the
    // overlay into the surface the display is scanning out, for a frame the
    // next flip immediately replaces - which is what was left flickering.
    EXPECT_FALSE(policy::ScanoutWriteIsPresentation(/*destOwnsFlipChain=*/true, 1));
}

TEST(DDrawPresentPolicyTest, WritesThatPileUpWithoutAFlipAreThePresentation) {
    // A loading screen: the 3D scene is not running, nothing flips, and the
    // writes are what the screen shows.
    EXPECT_TRUE(policy::ScanoutWriteIsPresentation(true, policy::kScanoutWritesWithoutFlipThreshold));
    EXPECT_TRUE(policy::ScanoutWriteIsPresentation(true, policy::kScanoutWritesWithoutFlipThreshold + 40));
}

TEST(DDrawPresentPolicyTest, AFlipResettingTheRunReturnsWritesToDrawing) {
    // Modelled the way the hook maintains it: a flip resets the run to zero and
    // the next write starts a new one.
    uint32_t writesSinceFlip = 12;
    EXPECT_TRUE(policy::ScanoutWriteIsPresentation(true, writesSinceFlip));
    writesSinceFlip = 0;  // a flip happened
    ++writesSinceFlip;
    EXPECT_FALSE(policy::ScanoutWriteIsPresentation(true, writesSinceFlip));
}

TEST(DDrawPresentPolicyTest, TheThresholdIsReachedOnTheSecondUnansweredWrite) {
    EXPECT_EQ(policy::kScanoutWritesWithoutFlipThreshold, 2u);
    EXPECT_FALSE(policy::ScanoutWriteIsPresentation(true, 1));
    EXPECT_TRUE(policy::ScanoutWriteIsPresentation(true, 2));
}
