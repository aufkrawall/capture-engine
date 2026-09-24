#pragma once

// Capture-target identity and source-loss truth policy. Pure decision logic
// (plus one latch register) shared by the media process's runtime retarget
// composition and the recording-health publication. Device-free so the
// contract is unit-testable without monitors or capture sessions.
//
// Core invariant: a recording resolved from a window target (or an explicit
// monitor selector) must never fall through to ChooseCandidate(kAuto) after
// its source dies. The kAuto chain (target window -> foreground window ->
// primary) would keep recording whatever the user is doing after the game
// exited.

#include <atomic>
#include <cstdint>

#include "capture_policy/constants.h"

namespace ce::capture_retarget {

// Where the active capture target was resolved from when it was primed.
enum class TargetOrigin : uint8_t {
    kAutoMonitor = 0,
    kWindowTarget,
    kExplicitMonitor,
};

constexpr const char* TargetOriginName(TargetOrigin origin) {
    switch (origin) {
        case TargetOrigin::kWindowTarget:
            return "window-target";
        case TargetOrigin::kExplicitMonitor:
            return "explicit-monitor";
        case TargetOrigin::kAutoMonitor:
            return "auto-monitor";
    }
    return "unknown";
}

// The selector kind a source-loss retarget may be composed with.
enum class RetargetSelector : uint8_t {
    kStopRecording = 0,  // no safe continuation; the recording must stop
    kPinnedMonitorId,    // re-resolve the cached monitor stable ID (same display)
    kAutoMonitor,        // monitor_selection kAuto chain (auto-origin targets only)
};

constexpr RetargetSelector SelectSourceLossRetarget(TargetOrigin origin, bool pinnedMonitorIdAvailable) {
    if (pinnedMonitorIdAvailable) {
        return RetargetSelector::kPinnedMonitorId;
    }
    return origin == TargetOrigin::kAutoMonitor ? RetargetSelector::kAutoMonitor
                                                : RetargetSelector::kStopRecording;
}

// Whether a failed in-recording retarget may roll back onto the previous
// capture. It may not when that capture is the requested window that died: the
// rollback republishes the dead window's capture, whose restart can "succeed"
// and then deliver no frames - a frozen recording under a healthy-looking
// session, which is exactly what the kStopRecording selector refused to allow.
constexpr bool ShouldRollBackFailedRetarget(bool recordingLive, bool requestedSourceLost) {
    return !(recordingLive && requestedSourceLost);
}

// Terminal action after a failed capture retarget while a recording is live.
enum class SourceLossRecovery : uint8_t {
    kNone = 0,      // no recording was affected
    kContinue,      // rollback restored a usable source; the recording is intact
    kStopDegraded,  // capture is definitively gone; latch degraded and stop
};

constexpr SourceLossRecovery SelectSourceLossRecovery(bool recordingLive, bool requestedSourceLost,
                                                      bool rollbackRestored) {
    if (!recordingLive) {
        return SourceLossRecovery::kNone;
    }
    if (requestedSourceLost || !rollbackRestored) {
        // Either the requested source is dead (no rollback onto it, see
        // ShouldRollBackFailedRetarget) or the rollback restart itself failed:
        // nothing usable is delivering frames any more, so the recording must
        // stop through the normal stop path and keep the committed prefix.
        return SourceLossRecovery::kStopDegraded;
    }
    return SourceLossRecovery::kContinue;
}

// Confirmed capture-source loss is always recorded as video-degraded truth:
// the committed output can no longer contain the requested source.
constexpr uint32_t kSourceLossHealthFlags = ce::capture_policy::kRecordingHealthFlagVideoDegraded;

// Latched source-loss truth. Recording-health publications overwrite their
// registers wholesale from the encoder-side state, so the latch lives here and
// is folded into every publication instead of being re-derived: no later
// observation can undo the fact that the requested source died mid-recording.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - trivially default-constructible atomic
inline std::atomic<uint32_t> g_SourceLossHealthFlags{0};

inline void ResetSourceLossHealth() {
    g_SourceLossHealthFlags.store(0, std::memory_order_release);
}

// Fold the latched truth into a recording-health flag word (encoder state or
// publication register) so later publications cannot drop it.
inline uint32_t WithLatchedSourceLossHealth(uint32_t flags) {
    return flags | (g_SourceLossHealthFlags.load(std::memory_order_acquire) &
                    ce::capture_policy::kRecordingHealthLatchedMask);
}

// Record confirmed capture-source loss: latch the degraded truth and publish
// it immediately into the session and overlay health registers.
inline void RecordSourceLossHealth(std::atomic<uint32_t>& sessionHealthFlags,
                                   std::atomic<uint32_t>* sharedHealthFlags) {
    g_SourceLossHealthFlags.fetch_or(kSourceLossHealthFlags, std::memory_order_release);
    sessionHealthFlags.fetch_or(kSourceLossHealthFlags, std::memory_order_release);
    if (sharedHealthFlags) {
        sharedHealthFlags->fetch_or(kSourceLossHealthFlags, std::memory_order_release);
    }
}

// Re-assert the latched truth into the health registers before recording
// finalization. The encoder thread's shutdown publications run between the
// owner thread's latch and finalization and would otherwise erase it.
inline void PublishLatchedSourceLossHealth(std::atomic<uint32_t>& sessionHealthFlags,
                                           std::atomic<uint32_t>* sharedHealthFlags) {
    const uint32_t latched =
        g_SourceLossHealthFlags.load(std::memory_order_acquire) & ce::capture_policy::kRecordingHealthLatchedMask;
    if (latched == 0) {
        return;
    }
    sessionHealthFlags.fetch_or(latched, std::memory_order_release);
    if (sharedHealthFlags) {
        sharedHealthFlags->fetch_or(latched, std::memory_order_release);
    }
}

// Delivered source textures are grouped into the format families the capture
// contract selects between (see WGCCapture::Impl::UpdateCaptureFormatSelection).
enum class SourceFormatFamily : uint8_t {
    kOther = 0,
    kEightBit,  // BGRA8 / RGBA8 SDR surfaces
    kTenBit,    // R10G10B10A2
    kFloat16,   // R16G16B16A16_FLOAT (scRGB HDR, or the SDR 10-bit pool fallback)
};

// True when a delivered source texture's format family crosses the HDR
// boundary of the capture contract, i.e. the display's advanced-color state
// changed under the capture and the stale HDR labeling/conversion must be
// confirmed immediately instead of waiting out the periodic probe. FP16 stays
// valid under an SDR contract when the pool itself was created FP16 (SDR
// 10-bit fallback), so the expected family participates in the decision.
constexpr bool SourceFormatFamilyContradictsHdrContract(SourceFormatFamily delivered, SourceFormatFamily expected,
                                                       bool captureIsHdr) {
    if (delivered == SourceFormatFamily::kOther || expected == SourceFormatFamily::kOther) {
        return false;
    }
    if (captureIsHdr) {
        return delivered != SourceFormatFamily::kFloat16;
    }
    return delivered == SourceFormatFamily::kFloat16 && expected != SourceFormatFamily::kFloat16;
}

}  // namespace ce::capture_retarget
