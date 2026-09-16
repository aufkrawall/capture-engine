#include <gtest/gtest.h>

#include <algorithm>
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
                                                /*captureIncludesOverlay=*/false,
                                                /*screenshotPending=*/false,
                                                /*screenshotIncludesOverlay=*/false));
    EXPECT_TRUE(policy::NativeOverlayShouldDraw(true, true, true, true, false, false));
    EXPECT_FALSE(policy::NativeOverlayShouldDraw(true, true, true, false, false, true));
    EXPECT_FALSE(policy::NativeOverlayShouldDraw(false, true, false, true, false, true));
    EXPECT_FALSE(policy::NativeOverlayShouldDraw(true, false, false, true, false, true));
}

TEST(DDrawPresentPolicyTest, NativeEndSceneRenderingHonorsOverlayExcludedScreenshot) {
    EXPECT_TRUE(policy::NativeOverlayShouldDraw(true, true, false, true,
                                                /*screenshotPending=*/true,
                                                /*screenshotIncludesOverlay=*/true));
    EXPECT_FALSE(policy::NativeOverlayShouldDraw(true, true, false, true,
                                                 /*screenshotPending=*/true,
                                                 /*screenshotIncludesOverlay=*/false));
    EXPECT_TRUE(policy::NativeOverlayShouldDraw(true, true, false, true,
                                                /*screenshotPending=*/false,
                                                /*screenshotIncludesOverlay=*/false));
}

TEST(DDrawPresentPolicyTest, ScreenshotOverlayOptionSelectsTheExactReadPhase) {
    EXPECT_EQ(policy::SelectOverlayReadPhase(/*requested=*/false, true, true),
              policy::OverlayReadPhase::None);
    EXPECT_EQ(policy::SelectOverlayReadPhase(/*requested=*/true, /*showOverlay=*/true,
                                             /*includeOverlay=*/false),
              policy::OverlayReadPhase::BeforeOverlay);
    EXPECT_EQ(policy::SelectOverlayReadPhase(/*requested=*/true, /*showOverlay=*/true,
                                             /*includeOverlay=*/true),
              policy::OverlayReadPhase::AfterOverlay);
    EXPECT_EQ(policy::SelectOverlayReadPhase(/*requested=*/true, /*showOverlay=*/false,
                                             /*includeOverlay=*/true),
              policy::OverlayReadPhase::BeforeOverlay);
}

TEST(DDrawPresentPolicyTest, RecordingAndScreenshotOverlayChoicesRemainIndependent) {
    for (bool captureIncludesOverlay : {false, true}) {
        for (bool screenshotIncludesOverlay : {false, true}) {
            const auto capture =
                policy::SelectOverlayReadPhase(true, true, captureIncludesOverlay);
            const auto screenshot =
                policy::SelectOverlayReadPhase(true, true, screenshotIncludesOverlay);
            EXPECT_EQ(capture == policy::OverlayReadPhase::AfterOverlay, captureIncludesOverlay);
            EXPECT_EQ(screenshot == policy::OverlayReadPhase::AfterOverlay, screenshotIncludesOverlay);
        }
    }
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

// Gothic II 20260916_005504: every application Flip failed for the whole
// session, so the success-gated heartbeat never armed the freeze watchdog
// (`renderLoopObserved=0` across 91 s and thousands of presentations). The
// watchdog then dumped a false-positive dialog while the game ran and refused
// to assert the freeze that followed. A returned presentation call is evidence
// of a live render loop whatever the runtime answered.
TEST(DDrawPresentPolicyTest, EveryPublishingKindCountsAsAPresentation) {
    using ce::ddraw_present_policy::PresentKind;
    EXPECT_TRUE(ce::ddraw_present_policy::PresentKindIsPresentation(PresentKind::FlipChain));
    EXPECT_TRUE(ce::ddraw_present_policy::PresentKindIsPresentation(PresentKind::BlitPresent));
    EXPECT_TRUE(ce::ddraw_present_policy::PresentKindIsPresentation(PresentKind::DirectScanout));
    // An ordinary drawing blit publishes nothing and must not arm anything.
    EXPECT_FALSE(ce::ddraw_present_policy::PresentKindIsPresentation(PresentKind::None));
}

TEST(DDrawPresentPolicyTest, PresentOperationsDescribeThemselvesForTheFailureLog) {
    using ce::ddraw_present_policy::PresentOperation;
    EXPECT_STREQ(ce::ddraw_present_policy::DescribePresentOperation(PresentOperation::Flip), "flip");
    EXPECT_STREQ(ce::ddraw_present_policy::DescribePresentOperation(PresentOperation::Blt), "blt");
    EXPECT_STREQ(ce::ddraw_present_policy::DescribePresentOperation(PresentOperation::BltFast), "bltfast");
    EXPECT_STREQ(ce::ddraw_present_policy::DescribePresentOperation(PresentOperation::None), "none");
}

// The CPU composite used to read, blend and store the surface one pixel at a
// time. It now copies a row out of video memory, computes over it in ordinary
// memory and copies it back; ComposeCompositeSpan is that arithmetic, and these
// tests pin the behaviour the per-pixel loop had.
namespace {

// The loop that stood in ddraw_hook_overlay_composite.cpp before the staged
// row copy, written out so the span has something independent to match.
void ReferenceComposeSpan(uint32_t* pixels, size_t count, const uint32_t* sprite, uint32_t* backdrop,
                          uint32_t* lastComposite, bool haveProof, bool restoreOnly) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t application = pixels[i] | 0xFF000000u;
        if (haveProof && application == lastComposite[i])
            application = backdrop[i];
        backdrop[i] = application;
        const uint32_t result =
            restoreOnly || !sprite ? application : policy::BlendPremultipliedOver(sprite[i], application);
        lastComposite[i] = result;
        pixels[i] = result;
    }
}

}  // namespace

TEST(DDrawPresentPolicyTest, ARepeatedCompositeWritesTheSamePixelsInsteadOfBlendingOverItself) {
    uint32_t backdrop[1] = {0u};
    uint32_t lastComposite[1] = {0u};
    const uint32_t sprite[1] = {0x80204060u};

    uint32_t pixels[1] = {0xFF102030u};
    policy::ComposeCompositeSpan(pixels, 1, sprite, backdrop, lastComposite, /*haveProof=*/false,
                                 /*restoreOnly=*/false);
    const uint32_t firstComposite = pixels[0];
    EXPECT_EQ(backdrop[0], 0xFF102030u);
    EXPECT_EQ(lastComposite[0], firstComposite);

    // The application has not drawn, so the surface still holds CE's output.
    policy::ComposeCompositeSpan(pixels, 1, sprite, backdrop, lastComposite, /*haveProof=*/true,
                                 /*restoreOnly=*/false);
    EXPECT_EQ(pixels[0], firstComposite);
    EXPECT_EQ(backdrop[0], 0xFF102030u);
}

TEST(DDrawPresentPolicyTest, AnApplicationWriteUnderTheOverlayBecomesTheNewBackdrop) {
    uint32_t backdrop[1] = {0xFF102030u};
    uint32_t lastComposite[1] = {0xFF445566u};
    const uint32_t sprite[1] = {0x80204060u};
    uint32_t pixels[1] = {0xFF778899u};

    policy::ComposeCompositeSpan(pixels, 1, sprite, backdrop, lastComposite, /*haveProof=*/true,
                                 /*restoreOnly=*/false);
    EXPECT_EQ(backdrop[0], 0xFF778899u);
    EXPECT_EQ(pixels[0], policy::BlendPremultipliedOver(sprite[0], 0xFF778899u));
}

TEST(DDrawPresentPolicyTest, ARestoringSpanPutsTheBackdropBackWithoutTheSprite) {
    uint32_t backdrop[1] = {0xFF102030u};
    uint32_t lastComposite[1] = {0xFF445566u};
    uint32_t pixels[1] = {0xFF445566u};

    policy::ComposeCompositeSpan(pixels, 1, nullptr, backdrop, lastComposite, /*haveProof=*/true,
                                 /*restoreOnly=*/true);
    EXPECT_EQ(pixels[0], 0xFF102030u);
    EXPECT_EQ(lastComposite[0], 0xFF102030u);
}

TEST(DDrawPresentPolicyTest, TheStagedSpanMatchesThePerPixelCompositeItReplaced) {
    constexpr size_t kCount = 64;
    uint32_t sprite[kCount] = {};
    uint32_t seedPixels[kCount] = {};
    uint32_t seedBackdrop[kCount] = {};
    uint32_t seedLastComposite[kCount] = {};
    uint32_t state = 0x1234567u;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    for (size_t i = 0; i < kCount; ++i) {
        sprite[i] = next();
        seedPixels[i] = next() | 0xFF000000u;
        seedBackdrop[i] = next() | 0xFF000000u;
        // Every fourth pixel is still exactly what CE wrote, which is the case
        // the proof exists for.
        seedLastComposite[i] = (i % 4 == 0) ? seedPixels[i] : (next() | 0xFF000000u);
    }

    for (int haveProof = 0; haveProof <= 1; ++haveProof) {
        for (int restoreOnly = 0; restoreOnly <= 1; ++restoreOnly) {
            uint32_t pixels[kCount] = {};
            uint32_t backdrop[kCount] = {};
            uint32_t lastComposite[kCount] = {};
            uint32_t referencePixels[kCount] = {};
            uint32_t referenceBackdrop[kCount] = {};
            uint32_t referenceLastComposite[kCount] = {};
            std::copy(std::begin(seedPixels), std::end(seedPixels), std::begin(pixels));
            std::copy(std::begin(seedBackdrop), std::end(seedBackdrop), std::begin(backdrop));
            std::copy(std::begin(seedLastComposite), std::end(seedLastComposite), std::begin(lastComposite));
            std::copy(std::begin(seedPixels), std::end(seedPixels), std::begin(referencePixels));
            std::copy(std::begin(seedBackdrop), std::end(seedBackdrop), std::begin(referenceBackdrop));
            std::copy(std::begin(seedLastComposite), std::end(seedLastComposite), std::begin(referenceLastComposite));

            policy::ComposeCompositeSpan(pixels, kCount, sprite, backdrop, lastComposite, haveProof != 0,
                                         restoreOnly != 0);
            ReferenceComposeSpan(referencePixels, kCount, sprite, referenceBackdrop, referenceLastComposite,
                                 haveProof != 0, restoreOnly != 0);

            for (size_t i = 0; i < kCount; ++i) {
                EXPECT_EQ(pixels[i], referencePixels[i]) << "pixel " << i;
                EXPECT_EQ(backdrop[i], referenceBackdrop[i]) << "backdrop " << i;
                EXPECT_EQ(lastComposite[i], referenceLastComposite[i]) << "lastComposite " << i;
            }
        }
    }
}

TEST(DDrawPresentPolicyTest, AStagedSpanWithoutStateDoesNothing) {
    uint32_t pixels[1] = {0xFF102030u};
    uint32_t lastComposite[1] = {0u};
    policy::ComposeCompositeSpan(pixels, 1, nullptr, nullptr, lastComposite, true, false);
    EXPECT_EQ(pixels[0], 0xFF102030u);
}

// Gothic II session 20260916_021049 named the caller on the first occurrence
// after the cycle report shipped: CE's saved Flip original was genuine
// DDRAW.dll code, and gameoverlayrenderer re-entered CE's detour from below it.
// The nested call was on the primary surface - the game's real screen flip - and
// returning DD_OK dropped one per frame, which is why the picture stopped
// updating while the 3D scene ran.
TEST(DDrawPresentPolicyTest, AnInjectorsEntryPatchIsRecognizedByItsJumpShape) {
    const unsigned char nearJump[] = {0xE9u, 0x12u, 0x34u, 0x56u, 0x78u};
    const unsigned char indirectJump[] = {0xFFu, 0x25u, 0x00u, 0x00u, 0x00u, 0x00u};
    const unsigned char ordinaryPrologue[] = {0x8Bu, 0xFFu, 0x55u, 0x8Bu, 0xECu};
    const unsigned char conditionalJump[] = {0x0Fu, 0x85u, 0x00u, 0x00u, 0x00u, 0x00u};

    EXPECT_TRUE(policy::EntryLooksInlinePatched(nearJump, sizeof(nearJump)));
    EXPECT_TRUE(policy::EntryLooksInlinePatched(indirectJump, sizeof(indirectJump)));
    EXPECT_FALSE(policy::EntryLooksInlinePatched(ordinaryPrologue, sizeof(ordinaryPrologue)));
    EXPECT_FALSE(policy::EntryLooksInlinePatched(conditionalJump, sizeof(conditionalJump)));

    // An FF byte alone is not a jump, and nothing is claimed for no bytes.
    const unsigned char truncated[] = {0xFFu};
    EXPECT_FALSE(policy::EntryLooksInlinePatched(truncated, sizeof(truncated)));
    EXPECT_FALSE(policy::EntryLooksInlinePatched(nullptr, 8));
    EXPECT_FALSE(policy::EntryLooksInlinePatched(nearJump, 0));
}

TEST(DDrawPresentPolicyTest, ANestedPresentationRunsTheRealImplementationExactlyOnce) {
    // The first nested call, with a bypass to use, is the frame that would
    // otherwise be lost.
    EXPECT_TRUE(policy::NestedPresentationMayRunRealImplementation(1, true, false));

    // Not twice on one thread: the second time would be a call from inside the
    // bypass, which is where an unbounded recursion would start.
    EXPECT_FALSE(policy::NestedPresentationMayRunRealImplementation(1, true, true));
    EXPECT_FALSE(policy::NestedPresentationMayRunRealImplementation(2, true, false));
    EXPECT_FALSE(policy::NestedPresentationMayRunRealImplementation(3, true, false));

    // With no bypass there is nothing safe to call: session 20260916_013230
    // answered the cycle with CE's own saved original and reached 32,768 levels
    // in two milliseconds.
    EXPECT_FALSE(policy::NestedPresentationMayRunRealImplementation(1, false, false));

    // The outermost presentation is not nested at all.
    EXPECT_FALSE(policy::NestedPresentationMayRunRealImplementation(0, true, false));
}
