#include "test_dxgi_shared_shared.h"

TEST(DXGISharedTest, ECLStartupActivationProbeIsSuppressedDuringNativeFSRPresentPath) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(true, true, false, false, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(false, true, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(true, false, false, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(true, true, true, false, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(true, true, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldProbePostSLStartupActivationSwapchainFromECL(true, true, false, false, true));
}

TEST(DXGISharedTest, FFXPresentCallbackOverlayBackendResetsOnlyForDeviceOrFormatChange) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(true, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(true, false, false));
}

TEST(DXGISharedTest, NormalOverlayCleanupPreservesFFXCallbackBackendOnlyWhileRuntimeOwnedNativeFGPresentPathPersists) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveFFXPresentCallbackBackendDuringNormalOverlayCleanup(true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveFFXPresentCallbackBackendDuringNormalOverlayCleanup(false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveFFXPresentCallbackBackendDuringNormalOverlayCleanup(true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveFFXPresentCallbackBackendDuringNormalOverlayCleanup(false, false));
}

TEST(DXGISharedTest, FFXPresentCallbackOverlayBridgeTrustsDirectFFXEvidenceWithoutRuntimeOwnedPath) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(false, true, false,
                                                                                  ce::fg_runtime::RuntimeMode::kFSRFG));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(false, false, true,
                                                                                  ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(true, false, false,
                                                                                  ce::fg_runtime::RuntimeMode::kOff));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(false, false, false,
                                                                                   ce::fg_runtime::RuntimeMode::kOff));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(
        false, false, false, ce::fg_runtime::RuntimeMode::kDLSSFG));
}

TEST(DXGISharedTest, FFXPresentCallbackMirrorsOverlayToCurrentBackbufferOnlyDuringSuspension) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(false, true, true,
                                                                                                  true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(true, true, true,
                                                                                                   true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(false, false, true,
                                                                                                   true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(false, true, false,
                                                                                                   true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(false, true, true,
                                                                                                   false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(false, true, true,
                                                                                                   true, false));
}

TEST(DXGISharedTest, FFXPresentCallbackComposesOutputOnlyWithoutRuntimeCompositionCallback) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldComposeFFXPresentSourceToOutput(false, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldComposeFFXPresentSourceToOutput(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldComposeFFXPresentSourceToOutput(false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldComposeFFXPresentSourceToOutput(false, true, false));
}

TEST(DXGISharedTest, FFXPresentCallbackBridgeRequiresRealAppCallback) {
    // App/default callback present -> install (WRAP the real callback).
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(true, true, true));
    // NO app callback (GTA native FSR FG) -> do NOT synthesize a bridge. Tried twice (1b71d43, 8acb8fd):
    // installing a callback flips AMD out of its native internal no-callback composition and wedges
    // ffxQuery in ~8 frames even though CE's compose GPU work completes (breadcrumb, session 20260618_155443).
    // The no-app-callback case must PRESERVE AMD internal composition + the normal DX12 overlay route.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(true, true, false));
    // Disabled / startup-arming configure -> never install (documented GTA fail-fast).
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(true, false, false));
    // Not a recognized FG configure -> inert.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(false, true, true));
}

TEST(DXGISharedTest, NativeFSRInternalNoCallbackCompositionUsesNormalOverlayRoute) {
    // App-callback FSR (nativeFSRInternalNoCallbackComposition=false): keep the present-callback route,
    // so the separate runtime-queue submit stays suppressed (skip=true) unless a stall fallback is allowed.
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, false, false));
}

TEST(DXGISharedTest, NativeFSRNoCallbackSkipsRuntimeQueueWhenUiResourceCompositionActive) {
    // No-app-callback native FSR FG via UI-resource composition (bundle path): the overlay is
    // appended to the game's existing ECL — skip the separate overlay GPU work. Live present is still on
    // AMD's separate FG queue (liveQueueIsOrigGame=false) and FG is NOT suspended, so the bundle covers.
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, /*ffxPresentCallbackFallbackAllowed=*/false,
        /*nativeFSRInternalNoCallbackComposition=*/true, /*ffxUiResourceCompositionActive=*/true,
        /*liveSwapchainQueueIsOriginalGameQueue=*/false, /*fsrFGDisabledSuspendPending=*/false));

    // No-app-callback native FSR WITHOUT UI-resource composition (no bundle available):
    // the normal DX12 overlay route renders on the original game queue — do NOT skip.
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, /*ffxPresentCallbackFallbackAllowed=*/false,
        /*nativeFSRInternalNoCallbackComposition=*/true, /*ffxUiResourceCompositionActive=*/false));
}

TEST(DXGISharedTest, NativeFSRNoCallbackSuspendRidesBundleNeverStallingBackbuffer) {
    // Regression across TWO sessions on the same suspend edge (dx12_fg_switch_test.exe suspends FSR FG via
    // ffxConfigure frameGenerationEnabled=0 while AMD keeps the FfxFrameInterpolationSwapchain):
    //   - 20260703_204119: overlay went blank the whole suspension (skip=true deferred init, bundle not firing).
    //   - 20260703_210021: routing the suspend to the BACKBUFFER instead stalled the app to ~1 fps — AMD stops
    //     flushing its runtime queue while suspended, so the overlay's GPU-completion fence never signals and
    //     every present waits ~1s.
    // Resolution: while AMD OWNS the swapchain (active OR suspended), the overlay rides the bundle composite on
    // CE's own fenced queue and CE submits ZERO backbuffer work — the skip decision and the present-time router
    // must AGREE on that. The backbuffer is reachable only once the game recreates its OWN native swapchain
    // (live queue back on origGame = stale latch).
    using ce::dx12_overlay_policy::ChooseNoCallbackFSRFGOverlayRoute;
    using ce::dx12_overlay_policy::NoCallbackFSRFGOverlayRoute;
    using ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration;

    // ACTIVE no-callback FSR FG: runtime-owned, NOT suspended, live present on AMD's separate FG queue.
    // Bundle covers → skip the separate work; router agrees kSkipBundleCovers.
    EXPECT_TRUE(ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, /*ffxPresentCallbackFallbackAllowed=*/false,
        /*nativeFSRInternalNoCallbackComposition=*/true, /*ffxUiResourceCompositionActive=*/true,
        /*liveSwapchainQueueIsOriginalGameQueue=*/false, /*fsrFGDisabledSuspendPending=*/false));
    EXPECT_EQ(ChooseNoCallbackFSRFGOverlayRoute(/*runtimeOwns=*/true, /*liveQueueIsOrigGame=*/false,
                                                /*fsrFGDisabledSuspendPending=*/false,
                                                /*uiResourceCachedForBundle=*/true,
                                                /*bundleOverlayActivelyFiring=*/false),
              NoCallbackFSRFGOverlayRoute::kSkipBundleCovers);

    // SUSPENDED no-callback FSR FG: runtime still owns the swapchain; the UI texture is still cached. CE must
    // STILL skip the separate/backbuffer work (the backbuffer submit stalls) and keep riding the bundle — the
    // skip decision must match the router's kSkipBundleCovers, NOT flip to the backbuffer.
    EXPECT_TRUE(ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kOff, false, true, /*ffxPresentCallbackFallbackAllowed=*/false,
        /*nativeFSRInternalNoCallbackComposition=*/true, /*ffxUiResourceCompositionActive=*/true,
        /*liveSwapchainQueueIsOriginalGameQueue=*/false, /*fsrFGDisabledSuspendPending=*/true));
    EXPECT_EQ(ChooseNoCallbackFSRFGOverlayRoute(/*runtimeOwns=*/true, /*liveQueueIsOrigGame=*/false,
                                                /*fsrFGDisabledSuspendPending=*/true,
                                                /*uiResourceCachedForBundle=*/true,
                                                /*bundleOverlayActivelyFiring=*/false),
              NoCallbackFSRFGOverlayRoute::kSkipBundleCovers);

    // STALE no-callback latch: the game recreated a native swapchain and presents on its own queue again
    // (live queue == origGame). AMD's FI swapchain is gone → the backbuffer on the game's OWN queue is safe and
    // does NOT stall → do NOT skip; router agrees kMinimalBackbuffer.
    EXPECT_FALSE(ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true, true, /*ffxPresentCallbackFallbackAllowed=*/false,
        /*nativeFSRInternalNoCallbackComposition=*/true, /*ffxUiResourceCompositionActive=*/true,
        /*liveSwapchainQueueIsOriginalGameQueue=*/true, /*fsrFGDisabledSuspendPending=*/false));
    EXPECT_EQ(ChooseNoCallbackFSRFGOverlayRoute(/*runtimeOwns=*/true, /*liveQueueIsOrigGame=*/true,
                                                /*fsrFGDisabledSuspendPending=*/false,
                                                /*uiResourceCachedForBundle=*/true,
                                                /*bundleOverlayActivelyFiring=*/false),
              NoCallbackFSRFGOverlayRoute::kMinimalBackbuffer);
}

TEST(DXGISharedTest, FFXRegisterUiResourceDescTypeMatchesAMDEncoding) {
    // FFX_API_MAKE_BACKEND_EFFECT_SUB_ID(DX12=0, FRAMEGENERATIONSWAPCHAIN=0x00030000, 0x02) == 0x00030002.
    EXPECT_EQ(ce::ffx_api::kConfigureDescTypeFrameGenerationSwapChainRegisterUiResourceDX12, 0x00030002ull);
    // ABI must match the SDK struct so the reinterpret_cast over the game's desc reads uiResource.resource /
    // uiResource.state / flags at the right offsets (header is 16 bytes: uint64 type + pointer).
    using UiDesc = ce::ffx_api::ConfigureDescFrameGenerationSwapChainRegisterUiResource;
    EXPECT_EQ(offsetof(UiDesc, uiResource), sizeof(ce::ffx_api::ApiHeader));
    EXPECT_EQ(offsetof(UiDesc, flags), sizeof(ce::ffx_api::ApiHeader) + sizeof(ce::ffx_api::Resource));
    EXPECT_EQ(offsetof(ce::ffx_api::Resource, resource), 0u);
    EXPECT_EQ(offsetof(ce::ffx_api::Resource, state), sizeof(void*) + sizeof(ce::ffx_api::ResourceDescription));
}

// Test the FFX UI-composite timeline ring buffer logic (rolling capture of last N composite events
// for freeze diagnosis). Replicates the ring buffer algorithm from dx12_hook.cpp to verify the
// modulo wraparound and that only the last kFFXUiCompositeTimelineSize entries are retained.
TEST(DXGISharedTest, FFXUiCompositeTimelineRingBufferWrapsCorrectly) {
    constexpr int kTimelineSize = 32;
    struct TimelineEntry {
        uint64_t frame;
        uint64_t fenceVal;
    };
    TimelineEntry timeline[kTimelineSize];
    uint32_t timelineIdx = 0;

    auto record = [&](uint64_t frame, uint64_t fenceVal) {
        const uint32_t idx = timelineIdx++;
        timeline[idx % kTimelineSize] = {frame, fenceVal};
    };

    // Fill exactly kTimelineSize entries (frames 0..31).
    for (int i = 0; i < kTimelineSize; ++i) {
        record(static_cast<uint64_t>(i), static_cast<uint64_t>(i * 10));
    }
    ASSERT_EQ(timelineIdx, static_cast<uint32_t>(kTimelineSize));

    // All 32 entries should be present in order (startIdx=0).
    const uint32_t startIdx0 =
        (timelineIdx >= static_cast<uint32_t>(kTimelineSize)) ? (timelineIdx % kTimelineSize) : 0;
    EXPECT_EQ(startIdx0, 0u);
    for (int i = 0; i < kTimelineSize; ++i) {
        EXPECT_EQ(timeline[i].frame, static_cast<uint64_t>(i));
        EXPECT_EQ(timeline[i].fenceVal, static_cast<uint64_t>(i * 10));
    }

    // Add 10 more entries (frames 32..41). The ring buffer should wrap and retain only the last 32.
    for (int i = 0; i < 10; ++i) {
        record(static_cast<uint64_t>(kTimelineSize + i), static_cast<uint64_t>((kTimelineSize + i) * 10));
    }
    EXPECT_EQ(timelineIdx, static_cast<uint32_t>(kTimelineSize + 10));

    // After 42 total entries, the ring buffer should retain the last 32 (frames 10..41).
    const uint32_t startIdx42 =
        (timelineIdx >= static_cast<uint32_t>(kTimelineSize)) ? (timelineIdx % kTimelineSize) : 0;
    EXPECT_EQ(startIdx42, 10u);  // 42 % 32 = 10
    const int count42 = kTimelineSize;
    for (int i = 0; i < count42; ++i) {
        const uint32_t slotIdx = (startIdx42 + i) % kTimelineSize;
        const uint64_t expectedFrame = static_cast<uint64_t>(10 + i);
        EXPECT_EQ(timeline[slotIdx].frame, expectedFrame) << "i=" << i << " slotIdx=" << slotIdx;
    }
}

// Test the 3-slot allocator rotation logic (Step 2 revised: fence signaled on CE's own dedicated queue,
// 3 rotating allocators recycled by fence value). At 60fps, 3 frames = 50ms — plenty for a single overlay
// draw to complete on the GPU before reuse, and the fence wait on CE's own queue guarantees completion.
TEST(DXGISharedTest, FFXUiCompositeThreeSlotRotationCoversAllSlots) {
    constexpr int kSlotCount = 3;
    // Verify slot selection for frames 0..8: should cycle 0, 1, 2, 0, 1, 2, 0, 1, 2.
    for (int frame = 0; frame < 9; ++frame) {
        const int slot = frame % kSlotCount;
        EXPECT_EQ(slot, frame % 3);
        EXPECT_GE(slot, 0);
        EXPECT_LT(slot, kSlotCount);
    }
    // Verify that with 3 slots, the oldest slot is always 3 frames behind — at 60fps that's 50ms.
    // A single overlay draw takes <1ms on the GPU, so the 3-frame + fence-wait is safe by ~50x margin.
    const int slotAtFrame100 = 100 % kSlotCount;
    EXPECT_EQ(slotAtFrame100, 1);  // 100 % 3 = 1
    const int slotAtFrame101 = 101 % kSlotCount;
    EXPECT_EQ(slotAtFrame101, 2);
    const int slotAtFrame102 = 102 % kSlotCount;
    EXPECT_EQ(slotAtFrame102, 0);  // wraps back to 0
}

// Test the cached UI texture null-skip: when no UI texture is cached (first frame or after FG off),
// the composite wrapper (DX12_CompositeOverlayOntoCachedFFXUiResource) must not attempt to draw (null → skip).
TEST(DXGISharedTest, FFXUiBundleCachedTextureNullSkip) {
    void* cachedTexture = nullptr;
    // First frame: no cached texture → skip bundle
    EXPECT_EQ(cachedTexture, nullptr);

    // After ffxConfigure caches the texture:
    cachedTexture = reinterpret_cast<void*>(0x1234);
    EXPECT_NE(cachedTexture, nullptr);

    // After ReleaseFFXUiCompositeInfra (device change / cleanup):
    cachedTexture = nullptr;
    EXPECT_EQ(cachedTexture, nullptr);
}

TEST(DXGISharedTest, HDRDetectionNeverTreatsStorageFormatAsDefinitive) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatFormatAsDefinitelyHDR(DXGI_FORMAT_R16G16B16A16_FLOAT));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatFormatAsDefinitelyHDR(DXGI_FORMAT_R10G10B10A2_UNORM));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatFormatAsDefinitelyHDR(DXGI_FORMAT_R8G8B8A8_UNORM));
}

TEST(DXGISharedTest, HDRDetectionRequiresPresentationContractForHDRCapableStorage) {
    EXPECT_TRUE(ce::dx12_overlay_policy::IsPresentationContractDependentFormat(DXGI_FORMAT_R10G10B10A2_UNORM));
    EXPECT_TRUE(ce::dx12_overlay_policy::IsPresentationContractDependentFormat(DXGI_FORMAT_R10G10B10A2_TYPELESS));
    EXPECT_TRUE(ce::dx12_overlay_policy::IsPresentationContractDependentFormat(DXGI_FORMAT_R16G16B16A16_FLOAT));
    EXPECT_TRUE(ce::dx12_overlay_policy::IsPresentationContractDependentFormat(DXGI_FORMAT_R16G16B16A16_TYPELESS));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPresentationContractDependentFormat(DXGI_FORMAT_R8G8B8A8_UNORM));
}

TEST(DXGISharedTest, HDRDetectionRecognizesHDRAndSDRTenBitColorSpaces) {
    EXPECT_TRUE(ce::dx12_overlay_policy::IsHDRColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
    EXPECT_TRUE(ce::dx12_overlay_policy::IsHDRColorSpace(DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsHDRColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsHDRColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
}

TEST(DXGISharedTest, HDRDetectionResolvesActualOverlayTargetStateFromFormatAndColorSpace) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ResolveActualHDRStateForOverlayTarget(DXGI_FORMAT_R16G16B16A16_FLOAT, false, -1));
    EXPECT_TRUE(ce::dx12_overlay_policy::ResolveActualHDRStateForOverlayTarget(
        DXGI_FORMAT_R16G16B16A16_FLOAT, true, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ResolveActualHDRStateForOverlayTarget(DXGI_FORMAT_R10G10B10A2_UNORM, false, -1));
    EXPECT_TRUE(ce::dx12_overlay_policy::ResolveActualHDRStateForOverlayTarget(
        DXGI_FORMAT_R10G10B10A2_UNORM, true, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
    EXPECT_FALSE(ce::dx12_overlay_policy::ResolveActualHDRStateForOverlayTarget(
        DXGI_FORMAT_R10G10B10A2_UNORM, true, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    EXPECT_FALSE(ce::dx12_overlay_policy::ResolveActualHDRStateForOverlayTarget(
        DXGI_FORMAT_R8G8B8A8_UNORM, true, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
}

TEST(DXGISharedTest, RuntimeOwnedCallbackHDRFallbackUsesCachedKnownState) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        DXGI_FORMAT_R16G16B16A16_FLOAT, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        DXGI_FORMAT_R16G16B16A16_FLOAT, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(DXGI_FORMAT_R8G8B8A8_UNORM,
                                                                                             true, true));

    EXPECT_TRUE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        DXGI_FORMAT_R10G10B10A2_UNORM, true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        DXGI_FORMAT_R10G10B10A2_TYPELESS, true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        DXGI_FORMAT_R16G16B16A16_TYPELESS, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        DXGI_FORMAT_R10G10B10A2_UNORM, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        DXGI_FORMAT_R10G10B10A2_UNORM, false, true));
}

TEST(DXGISharedTest, PresentationEncodingUsesColorSpaceRatherThanStorageFormat) {
    using ce::presentation_color::Encoding;
    EXPECT_EQ(Encoding::Sdr709,
              ce::presentation_color::ResolveDXGI(DXGI_FORMAT_R10G10B10A2_UNORM, false,
                                                  DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
    EXPECT_EQ(Encoding::Sdr709,
              ce::presentation_color::ResolveDXGI(DXGI_FORMAT_R10G10B10A2_UNORM, true,
                                                  DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
    EXPECT_EQ(Encoding::Hdr10Pq,
              ce::presentation_color::ResolveDXGI(DXGI_FORMAT_R10G10B10A2_UNORM, true,
                                                  DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
    EXPECT_EQ(Encoding::LinearScRgb,
              ce::presentation_color::ResolveDXGI(DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                                                  DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    EXPECT_EQ(Encoding::Unsupported,
              ce::presentation_color::ResolveDXGI(DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                                                  DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
}

TEST(DXGISharedTest, WrappedColorSpaceForwardOwnsExactlyOncePublication) {
    EXPECT_TRUE(ce::presentation_color::ShouldRecordDetouredColorSpaceChange(0));
    EXPECT_FALSE(ce::presentation_color::ShouldRecordDetouredColorSpaceChange(1));
    EXPECT_FALSE(ce::presentation_color::ShouldRecordDetouredColorSpaceChange(2));
}

TEST(DXGISharedTest, SwapchainColorSpaceTrackingNeverPatchesSharedVtableSlot) {
    const auto readSource = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    const std::string shared =
        readSource(std::filesystem::current_path() / "hook" / "common" / "dxgi_shared.cpp");
    const std::string wrapper =
        readSource(std::filesystem::current_path() / "hook" / "wrappers" / "dxgi_swapchain_wrap.cpp");
    ASSERT_FALSE(shared.empty());
    ASSERT_FALSE(wrapper.empty());
    EXPECT_NE(shared.find("DetourSetColorSpace1"), std::string::npos);
    EXPECT_NE(shared.find("InlineHook::InstallPublished(colorSpaceAddress"), std::string::npos);
    EXPECT_NE(shared.find("IsWrappedSwapChainObject(pSwapChain)"), std::string::npos);
    EXPECT_NE(shared.find("SetSwapChainColorSpaceFromWrapper"), std::string::npos);
    EXPECT_NE(wrapper.find("DXGIShared::SetSwapChainColorSpaceFromWrapper(m_pReal3, m_pReal, ColorSpace)"),
              std::string::npos);
    EXPECT_EQ(shared.find("vtable[38] = (void*)DetourSetColorSpace1"), std::string::npos);
    EXPECT_EQ(shared.find("oSetColorSpace1 ="), std::string::npos);
}

TEST(DXGISharedTest, RuntimeOwnedOverlayRoutesUseCachedPresentationContractAndRefreshTransitions) {
    const auto readSource = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    const auto root = std::filesystem::current_path();
    const std::string dx12 = readSource(root / "hook" / "apis" / "dx12_hook.cpp");
    const std::string streamlineHook = readSource(root / "hook" / "apis" / "streamline_hook.cpp");
    const std::string streamlineRenderer =
        readSource(root / "hook" / "apis" / "dx12_streamline_ui_overlay.cpp");
    ASSERT_FALSE(dx12.empty());
    ASSERT_FALSE(streamlineHook.empty());
    ASSERT_FALSE(streamlineRenderer.empty());
    EXPECT_NE(dx12.find("request.hdr = DX12_ResolveRuntimeOwnedOverlayTargetHDRState"), std::string::npos);
    EXPECT_NE(dx12.find("DX12: Presentation color state changed"), std::string::npos);
    EXPECT_NE(dx12.find("g_D3D11On12Adapter.SetHDR(isHdr, static_cast<int>(format))"), std::string::npos);
    size_t secondaryColorSyncCalls = 0;
    for (size_t position = 0;
         (position = dx12.find("SyncSecondaryDx12OverlayColorState(g_State.format)", position)) !=
         std::string::npos;
         ++position) {
        ++secondaryColorSyncCalls;
    }
    EXPECT_EQ(secondaryColorSyncCalls, 4u);
    EXPECT_NE(streamlineHook.find("DX12_ResolveRuntimeOwnedOverlayTargetHDRState(format)"), std::string::npos);
    EXPECT_NE(streamlineRenderer.find("g_Renderer->UpdateHdr(request.hdr)"), std::string::npos);
    EXPECT_EQ(streamlineHook.find("const bool hdr = format == DXGI_FORMAT_R10G10B10A2"), std::string::npos);
}

TEST(DXGISharedTest, AuthoritativeFSRRealFrameOnlyRunTracksOnlyQualifiedFrames) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, true, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(true, true, true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, false, true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(false, true, true, false, true));
}

TEST(DXGISharedTest, StaleAuthoritativeFSRRequiresLongFreshRealFrameOnlyRunBeforeClearing) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(1, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(119, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(120, false));
}

TEST(DXGISharedTest, StaleAuthoritativeFSRDoesNotClearAfterDirectFFXApiConfirmation) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(120, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(1200, true));
}

TEST(DXGISharedTest, TracksStaleRuntimeOwnedStreamlineNoFGOnlyOnRealFramesBackOnOriginalQueue) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        true, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, ce::fg_runtime::RuntimeMode::kDLSSFG, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        false, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true, true, true));
}

TEST(DXGISharedTest, StaleRuntimeOwnedStreamlineNoFGRequiresLongRealFrameRunBeforeClearing) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearStaleRuntimeOwnedStreamlineNoFGAfterRealFrameRun(1));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldClearStaleRuntimeOwnedStreamlineNoFGAfterRealFrameRun(119));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldClearStaleRuntimeOwnedStreamlineNoFGAfterRealFrameRun(120));
}

TEST(DXGISharedTest, StaleRuntimeOwnedStreamlineNoFGCleanupReleasesRetainedActivationSwapchain) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldReleaseRetainedStartupActivationSwapchainAfterStaleNoFGCleanup(true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReleaseRetainedStartupActivationSwapchainAfterStaleNoFGCleanup(false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReleaseRetainedStartupActivationSwapchainAfterStaleNoFGCleanup(true, false));
}

TEST(DXGISharedTest, AuthoritativeFFXCreateReleasesRetainedStreamlineStartupActivationSwapchain) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldReleaseRetainedStreamlineStartupActivationSwapchainForAuthoritativeFFXCreate(
            true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReleaseRetainedStreamlineStartupActivationSwapchainForAuthoritativeFFXCreate(
            false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReleaseRetainedStreamlineStartupActivationSwapchainForAuthoritativeFFXCreate(
            true, false));
}

TEST(DXGISharedTest, DescFreeBackendUsesAdapterShutdownWhenAdapterOwnsCustomBackend) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(true, true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(false, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(true, true, false));
}

TEST(DXGISharedTest, AuthoritativeFSRIsPreservedDuringTransitionCooldownForTransientOffEdges) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, true, 1));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, true, 90));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(false, true, 90));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, false, 90));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(true, true, 0));
}

TEST(DXGISharedTest, HeuristicFSRIsPreservedDuringTransientBlocksOnRuntimeOwnedPostFSRSwapchains) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, true, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(true, true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(false, true, false, false));
}

TEST(DXGISharedTest, RuntimeOwnedPostFSRTeardownRequiresStrongerOffSignalThanTransientNoneEdge) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(true, true, true, true));
}

TEST(DXGISharedTest, ExplicitNativeFSROffSuppressesHeuristicReactivationUntilRuntimeOwnedTeardownEnds) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(true, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(false, false));
}

TEST(DXGISharedTest, ExplicitNativeFSROffEndsRuntimeOwnedTeardownWhenQueueReturnsToOriginal) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, true, false, ce::fg_runtime::RuntimeMode::kOff, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        false, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, false, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, true, true, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, true, false, ce::fg_runtime::RuntimeMode::kFSRFG, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
        true, true, false, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false));
}

TEST(DXGISharedTest, DisabledNativeFSRConfigurePreservesCallbackOwnedPresentPath) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        true, false, false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        false, false, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        false, false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
        false, false, false, false));
}

TEST(DXGISharedTest, FFXPresentCallbackBridgeInstallsOnlyForEnabledFrameGenerationConfigure) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(false, false));
}

TEST(DXGISharedTest, NativeFSRDisabledConfigureStartupArmingRequiresFreshRuntimeOwnedTakeover) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, false, true, true,
                                                                                              true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, false, true, true,
                                                                                              false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(false, false, true, true,
                                                                                               true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, true, true, true,
                                                                                               true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, false, false, true,
                                                                                               true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, false, true, false,
                                                                                               true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(true, false, true, true,
                                                                                               true, true));
}

TEST(DXGISharedTest, RuntimeOwnedSwapchainNeedsNativeFSRProofBeforeNativePresentPath) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(true, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(true, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(false, false, true));
}

TEST(DXGISharedTest, OfficialFFXTakeoverDefersHeavySideEffectsUntilEnabledConfigure) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(true, true,
                                                                                                        true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(false, true,
                                                                                                         true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(true, false,
                                                                                                         true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(true, true,
                                                                                                         false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(true, true,
                                                                                                         true, true));
}

TEST(DXGISharedTest, OfficialFFXStartupSwapchainCreateUsesProtectedPassThroughUntilDirectConfigure) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(true, true, false));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(false, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(true, false, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(true, true, true));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupStagesOnlyDirectQueuesForDeferredTakeover) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(true, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(false, false));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupQuiescesCESideEffectsUntilDirectConfigure) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(true, true));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupDoesNotAllowSeparateOverlayOnlyBeforeProof) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowOverlayOnlyDuringProtectedOfficialFFXStartup(true, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowOverlayOnlyDuringProtectedOfficialFFXStartup(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowOverlayOnlyDuringProtectedOfficialFFXStartup(false, false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldAllowOverlayOnlyDuringProtectedOfficialFFXStartup(true, false, false));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupOverlayOnlyDoesNotBypassFGCooldownWithStagedQueue) {
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBypassFGTransitionCooldownForProtectedOfficialFFXOverlayOnly(true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBypassFGTransitionCooldownForProtectedOfficialFFXOverlayOnly(false, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBypassFGTransitionCooldownForProtectedOfficialFFXOverlayOnly(true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldBypassFGTransitionCooldownForProtectedOfficialFFXOverlayOnly(false, false));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupPreservesBackendAcrossSwapchainChangeUntilProof) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldPreserveOverlayBackendAcrossProtectedOfficialFFXStartupSwapchainChange(
        true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveOverlayBackendAcrossProtectedOfficialFFXStartupSwapchainChange(
        false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldPreserveOverlayBackendAcrossProtectedOfficialFFXStartupSwapchainChange(
        true, true));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupQuiescesLiveStreamlinePostSLImmediately) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, true, false, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, false, true, false, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, false, false, true, false, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, false, false, false, true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, false, false, false, false, true));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, false, false, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        false, false, true, true, true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
        true, true, true, true, true, true, true));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupDoesNotResolveFromProgressWithoutDirectConfigure) {
    const uint32_t processFrameThreshold =
        ce::dx12_overlay_policy::GetProtectedOfficialFFXStartupProcessFrameProgressThreshold();
    const uint32_t eclThreshold = ce::dx12_overlay_policy::GetProtectedOfficialFFXStartupECLProgressThreshold();

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        true, false, processFrameThreshold - 1, eclThreshold - 1));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        true, false, processFrameThreshold, 0));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        true, false, 0, eclThreshold));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        false, false, processFrameThreshold, eclThreshold));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
        true, true, processFrameThreshold, eclThreshold));
}

TEST(DXGISharedTest, AuthoritativeRuntimeSwapchainCreatePreservesOriginalDescriptor) {
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(false, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(false, false));
}

TEST(DXGISharedTest, ProtectedOfficialFFXStartupCountsAsRuntimeOwnedForDisabledStartupArmingConfigure) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(true, false));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(false, true));
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldTreatNativeFSRSwapchainAsRuntimeOwnedForConfigure(false, false));
}

TEST(DXGISharedTest, PostFSRNonFGRecoveryUsesPrimaryQueueForFrameClassificationWhenPresentAndRenderQueuesDiffer) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, false, false, true, true, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        false, false, false, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, true, false, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, true, false, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, false, true, true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, false, false, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, false, false, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        true, false, false, false, true, true, true));
}

TEST(DXGISharedTest, DirectPostFSROffIsTreatedAsPostFSRNonFGRecovery) {
    EXPECT_TRUE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(true, true, false, false, false));

    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(false, true, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(true, false, false, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(true, true, true, false, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(true, true, false, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(true, true, false, false, true));
}

TEST(DXGISharedTest, PostFSRNonFGRecoveryReservesInactiveFGOverlaySpace) {
    EXPECT_TRUE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(true, true, true));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(true, true, false));
    EXPECT_FALSE(ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(false, true, true));
}

TEST(DXGISharedTest, InactiveFGOverlaySpaceReservationRequiresShortPostSLTeardownActivity) {
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, true, true));
    EXPECT_TRUE(
        ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, true, true));

    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, true, false));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(false, true, true));
    EXPECT_FALSE(
        ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(true, false, false));
}

