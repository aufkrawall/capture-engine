#pragma once

#include <atomic>
#include <cstdint>

// Presentation diagnostics and bootstrap state are shared by the DirectDraw
// detours and the narrowly included presentation-override implementation. Keep
// them independent from ddraw_hook_internal.h so adding a focused translation
// unit does not pull every overlay and limiter implementation header into it.
struct DDrawPresentationDiagnostics {
    std::atomic<uint32_t> flips{0};
    std::atomic<uint32_t> blitPresents{0};
    std::atomic<uint32_t> directScanoutBlits{0};
    std::atomic<uint32_t> ignoredBlits{0};
    std::atomic<uint32_t> scanoutUnlocks{0};
    std::atomic<uint32_t> composites{0};
    std::atomic<uint32_t> skippedNoPublishedImage{0};
    std::atomic<uint32_t> skippedOutsideOverlay{0};
    std::atomic<uint32_t> scanoutWritesLeftToFlip{0};
    std::atomic<uint32_t> compositeSucceeded{0};
    std::atomic<uint32_t> compositeNoGeometry{0};
    std::atomic<uint32_t> compositeWriteFailed{0};
    std::atomic<uint32_t> reentrantPresentations{0};
    std::atomic<uint32_t> spriteRasterizations{0};
    std::atomic<uint32_t> spriteReuses{0};
    std::atomic<uint32_t> compositesSkippedClean{0};
    std::atomic<uint32_t> compositePartialWrites{0};
    std::atomic<uint32_t> compositeFullWrites{0};
    std::atomic<uint32_t> applicationWritesMarked{0};
    std::atomic<uint32_t> primaryUnlockPresentations{0};
    std::atomic<uint32_t> getDcPresentations{0};
    std::atomic<uint32_t> surfaceCreations{0};
    std::atomic<uint32_t> surfaceHookReuses{0};
    std::atomic<uint32_t> vblankWaits{0};
    std::atomic<uint32_t> vblankWaitFailures{0};
    std::atomic<uint32_t> applicationVblankWaitsReused{0};
    std::atomic<uint32_t> prerenderChecks{0};
    std::atomic<uint32_t> prerenderWaits{0};
    std::atomic<uint32_t> prerenderWaitFailures{0};
    std::atomic<uint32_t> nativeSceneDraws{0};
    std::atomic<uint32_t> nativePresentations{0};
    std::atomic<uint32_t> nativeDrawFailures{0};
    std::atomic<uint32_t> nativeRepairRegions{0};
    std::atomic<uint32_t> nativeUnsafeDeferrals{0};
    std::atomic<uint32_t> captureNativeDeferrals{0};
    std::atomic<uint32_t> spriteFullRasters{0};
    std::atomic<uint32_t> spriteIncrementalUpdates{0};
    std::atomic<uint32_t> rasterPasses{0};
    std::atomic<uint32_t> surfaceWritePasses{0};
    std::atomic<uint32_t> compositeTimedPresentations{0};
    std::atomic<uint64_t> rasterMicrosecondsTotal{0};
    std::atomic<uint32_t> rasterMicrosecondsMax{0};
    std::atomic<uint64_t> lockMicrosecondsTotal{0};
    std::atomic<uint32_t> lockMicrosecondsMax{0};
    std::atomic<uint64_t> writeMicrosecondsTotal{0};
    std::atomic<uint32_t> writeMicrosecondsMax{0};
    std::atomic<uint64_t> compositeMicrosecondsTotal{0};
    std::atomic<uint32_t> compositeMicrosecondsMax{0};
    std::atomic<uint64_t> vblankWaitMicrosecondsTotal{0};
    std::atomic<uint32_t> vblankWaitMicrosecondsMax{0};
    std::atomic<uint64_t> prerenderWaitMicrosecondsTotal{0};
    std::atomic<uint32_t> prerenderWaitMicrosecondsMax{0};
    std::atomic<uint32_t> lastLogTick{0};
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - atomic members are constant-initialized
inline DDrawPresentationDiagnostics ddraw_hook_g_PresentationDiagnostics;

inline thread_local unsigned ddraw_hook_g_DDrawBootstrapDepth = 0;
