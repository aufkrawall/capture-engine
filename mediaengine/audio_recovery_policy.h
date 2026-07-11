#pragma once

#include <cstdint>

// Pure, OS-independent decision helpers for recovering a WASAPI capture stream
// that has died mid-recording.
//
// Two real failure modes motivate this (both observed with per-process / app
// audio loopback, which is bound to the target process's audio-session
// lifecycle and is far more fragile than endpoint loopback):
//
//   1. A FATAL stream error such as AUDCLNT_E_DEVICE_INVALIDATED. The underlying
//      render endpoint changed (default-device switch, driver/power event,
//      HDMI/Bluetooth/USB-DAC reset over a long session). Every subsequent
//      GetNextPacketSize/GetBuffer call then fails forever and the capture loop
//      would otherwise spin without ever producing another packet.
//
//   2. A SILENT STALL with no error HRESULT at all. The target app tore down and
//      recreated its audio session (e.g. a game that auto-mutes when alt-tabbed
//      out and unmutes when refocused). The process-loopback mixer does not
//      always reattach to the new session, so the stream stays alive but yields
//      zero packets indefinitely even though the process is running and audible
//      again.
//
// In both cases the fix is the same: tear down and re-activate the client in
// place, keeping the capture thread / queue / downstream track alive. These
// helpers decide WHEN to attempt that, with exponential backoff so a stream that
// cannot be recovered (or an app that is legitimately paused) cannot be hammered.
//
// The constants below are generic safety policy, not device- or system-specific
// tuning: they bound how aggressively recovery is attempted on ANY machine.

namespace ce::audio {

// Re-activation timing policy. Defaults are conservative, universal bounds.
struct StreamRecoveryConfig {
    // A process-loopback stream that produced audio before, then yields zero
    // packets for at least this long while its target process is still running,
    // is treated as a dead-but-silent stall and re-activated.
    uint64_t silentStallReactivateMs = 10000;
    // First retry waits this long; each subsequent retry doubles up to the cap.
    uint64_t baseBackoffMs = 1000;
    uint64_t maxBackoffMs = 30000;
};

// AUDCLNT_E_* HRESULTs (stored as negative HRESULT / 0x8889xxxx when unsigned)
// that mean "the client is gone, re-activate from scratch" rather than a
// transient hiccup we can retry on the same client. Kept deliberately narrow so
// we never re-activate on benign/transient conditions.
inline bool IsFatalWasapiStreamError(long hr) {
    if (hr >= 0) {
        return false;  // SUCCEEDED
    }
    switch (static_cast<uint32_t>(hr)) {
        case 0x88890001u:  // AUDCLNT_E_NOT_INITIALIZED
        case 0x88890004u:  // AUDCLNT_E_DEVICE_INVALIDATED
        case 0x8889000fu:  // AUDCLNT_E_ENDPOINT_CREATE_FAILED
        case 0x88890010u:  // AUDCLNT_E_SERVICE_NOT_RUNNING
        case 0x88890026u:  // AUDCLNT_E_RESOURCES_INVALIDATED
            return true;
        default:
            return false;
    }
}

// Exponential backoff progression: 0 -> base, then doubling, capped at max.
inline uint64_t NextRecoveryBackoffMs(uint64_t currentBackoffMs, const StreamRecoveryConfig& cfg) {
    if (currentBackoffMs == 0) {
        return cfg.baseBackoffMs;
    }
    const uint64_t doubled = currentBackoffMs * 2;
    return doubled > cfg.maxBackoffMs ? cfg.maxBackoffMs : doubled;
}

// True once enough wall-clock time has passed since the last re-activation
// attempt to try again. A zero lastReactivateTickMs means "never attempted".
// A backwards clock (now < last) defers rather than firing early.
inline bool RecoveryBackoffElapsed(uint64_t nowMs, uint64_t lastReactivateTickMs, uint64_t currentBackoffMs) {
    if (lastReactivateTickMs == 0) {
        return true;
    }
    if (nowMs < lastReactivateTickMs) {
        return false;
    }
    return (nowMs - lastReactivateTickMs) >= currentBackoffMs;
}

// Silent-stall watchdog gate. Only arms after the stream has delivered at least
// one packet (sawAnyPacket), so a source whose process is merely paused from the
// start is never churned. Then requires the configured silent window AND that
// backoff has elapsed since the previous attempt.
inline bool ShouldReactivateForSilentStall(bool sawAnyPacket, uint64_t nowMs, uint64_t lastPacketTickMs,
                                           uint64_t lastReactivateTickMs, uint64_t currentBackoffMs,
                                           const StreamRecoveryConfig& cfg) {
    if (!sawAnyPacket) {
        return false;
    }
    if (nowMs < lastPacketTickMs) {
        return false;
    }
    if ((nowMs - lastPacketTickMs) < cfg.silentStallReactivateMs) {
        return false;
    }
    return RecoveryBackoffElapsed(nowMs, lastReactivateTickMs, currentBackoffMs);
}

}  // namespace ce::audio
